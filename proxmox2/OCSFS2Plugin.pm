package PVE::Storage::Custom::OCSFS2Plugin;

# SPDX-License-Identifier: GPL-2.0-only
#
# OCSFS v2 storage plugin for Proxmox VE.
#
# Mounts a shared SAN LUN with the OCSFS v2 filesystem (single-writer ownership)
# and presents it as a normal PVE datastore for all content types. It behaves
# like the built-in directory storage (so VM/CT image management, backups, ISOs
# all work) but owns the mount/unmount of the clustered device and prefers
# reflink for fast clones.
#
# /etc/pve/storage.cfg:
#
#   ocsfs2: vmstore
#       path /mnt/pve/vmstore
#       device /dev/disk/by-id/scsi-3600...        # the shared LUN (stable path)
#       content images,iso,vztmpl,backup,rootdir,snippets
#       cluster 1                                  # mount -o cluster (multinode)
#       shared 1
#
# Offline migration works with no data copy — the write-ownership lease passes
# from source to destination on close/open. ONLINE migration (qm migrate
# --online) is NOT yet supported: it needs the destination QEMU to open the disk
# while the source still holds the EX lease, and the lease currently has no
# revocation (the destination open returns -EBUSY). Tracked in docs/TODO.md.

use strict;
use warnings;

use PVE::Tools qw(run_command file_read_firstline);
use PVE::Storage::Plugin;
use PVE::JSONSchema qw(get_standard_option);

use base qw(PVE::Storage::Plugin);

my $MODULE = 'ocsfs2';

# ── registration ──
sub api { return 14; }            # PVE 9.x storage API (APIVER 14); base provides
                                  # defaults for the newer methods we don't override
sub type { return 'ocsfs2'; }

sub plugindata {
    return {
        content => [
            { images => 1, rootdir => 1, vztmpl => 1,
              iso => 1, backup => 1, snippets => 1 },
            { images => 1, rootdir => 1 },
        ],
        format => [ { raw => 1, qcow2 => 1, subvol => 0 }, 'raw' ],
    };
}

sub properties {
    return {
        device => {
            description => "Shared block device (SAN LUN) holding the OCSFS2 volume. "
                . "Use a stable /dev/disk/by-id or by-path name.",
            type => 'string',
        },
        cluster => {
            description => "Mount -o cluster (multinode single-writer ownership). "
                . "Set on a LUN shared by >1 node; omit for a single host.",
            type => 'boolean',
        },
    };
}

sub options {
    return {
        path    => { fixed => 1 },
        device  => { fixed => 1 },
        cluster => { optional => 1 },
        content => { optional => 1 },
        shared  => { optional => 1 },
        disable => { optional => 1 },
        nodes   => { optional => 1 },
        'prune-backups' => { optional => 1 },
        'max-protected-backups' => { optional => 1 },
    };
}

# ── mount management ──
sub _is_mounted {
    my ($path) = @_;
    open(my $fh, '<', '/proc/self/mountinfo') or return 0;
    while (my $l = <$fh>) {
        my @f = split(/\s+/, $l);
        # mountinfo: field 5 is the mount point; fs type follows " - "
        if (defined $f[4] && $f[4] eq $path && $l =~ / - \Q$MODULE\E /) {
            close($fh); return 1;
        }
    }
    close($fh);
    return 0;
}

sub activate_storage {
    my ($class, $storeid, $scfg, $cache) = @_;

    my $path = $scfg->{path};
    return if _is_mounted($path);

    my $device = $scfg->{device}
        or die "ocsfs2 storage '$storeid': no 'device' configured\n";
    die "ocsfs2 storage '$storeid': device '$device' not found\n" if !-e $device;

    # ensure the kernel module is present (out-of-tree; depmod-installed)
    system('modprobe', $MODULE);

    mkdir $path if !-d $path;

    my @cmd = ('mount', '-t', $MODULE);
    push @cmd, ('-o', 'cluster') if $scfg->{cluster};
    push @cmd, ($device, $path);
    run_command(\@cmd, errmsg => "mount $MODULE on '$path'");

    $class->SUPER::activate_storage($storeid, $scfg, $cache);
}

sub deactivate_storage {
    my ($class, $storeid, $scfg, $cache) = @_;
    my $path = $scfg->{path};
    if (_is_mounted($path)) {
        run_command(['umount', $path], errmsg => "umount $MODULE '$path'");
    }
}

# ── fast clone via reflink — for ANY format (raw or qcow2). `cp --reflink` of a
# qcow2 file shares its blocks (CoW on write) just like raw, so the clone is
# instant and space-efficient instead of a full qemu-img copy. Fall back to the
# base behaviour only for snapshot-based (linked) clones. ──
sub clone_image {
    my ($class, $scfg, $storeid, $volname, $vmid, $snap) = @_;

    my ($vtype, $name, $ownervm, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    if (!$snap) {
        my $src = $class->filesystem_path($scfg, $volname);
        # $add_fmt_suffix=1: qcow2 volnames MUST carry the .qcow2 suffix or
        # parse_volname rejects them ("unable to parse volume filename").
        my $newname = $class->find_free_diskname($storeid, $scfg, $vmid, $fmt, 1);
        my $dstvol = "$vmid/$newname";
        my $dst = $class->filesystem_path($scfg, $dstvol);
        run_command(['cp', '--reflink=always', $src, $dst],
                    errmsg => "ocsfs2 reflink clone failed");
        return $dstvol;
    }
    return $class->SUPER::clone_image($scfg, $storeid, $volname, $vmid, $snap);
}

1;
