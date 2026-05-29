package PVE::Storage::OCSFSPlugin;

# SPDX-License-Identifier: AGPL-3.0-or-later
#
# OCSFS Storage Plugin for Proxmox VE
#
# Provides a first-class storage backend for OCSFS cluster filesystems.
# Supports all PVE content types on a single shared datastore:
#   - VM disk images (raw, qcow2)
#   - ISO images
#   - Container templates
#   - VZDump backups
#   - Container rootfs
#   - Snippets (cloud-init, hookscripts)
#
# Configuration in /etc/pve/storage.cfg:
#
#   ocsfs: fc-shared
#     path /mnt/pve/fc-shared
#     device /dev/mapper/mpath-3600508b...
#     content images,iso,vztmpl,backup,rootdir,snippets
#     maxnodes 16
#     thin 1
#     shared 1
#

use strict;
use warnings;

use PVE::Storage::Plugin;
use PVE::Tools qw(run_command file_read_firstline);
use PVE::JSONSchema qw(get_standard_option);
use File::Path qw(make_path);
use File::Basename;
use POSIX qw(ceil);

use base qw(PVE::Storage::Plugin);

# ═══════════════════════════════════════════════════════════════
# PLUGIN REGISTRATION
# ═══════════════════════════════════════════════════════════════

sub type {
    return 'ocsfs';
}

sub plugindata {
    return {
        content => [
            { images   => 1, rootdir  => 1, vztmpl => 1,
              iso      => 1, backup   => 1, snippets => 1 },
            { images   => 1, rootdir  => 1 },
        ],
        format => [ { raw => 1, qcow2 => 1 }, 'raw' ],
    };
}

# ═══════════════════════════════════════════════════════════════
# STORAGE CONFIGURATION PROPERTIES
# ═══════════════════════════════════════════════════════════════

sub properties {
    return {
        device => {
            description => "Block device path (SAN LUN or loopback).",
            type        => 'string',
        },
        maxnodes => {
            description => "Maximum number of cluster nodes (1-256).",
            type        => 'integer',
            minimum     => 1,
            maximum     => 256,
            default     => 64,
        },
        thin => {
            description => "Enable thin provisioning for new VM disks.",
            type        => 'boolean',
            default     => 1,
        },
        extentsize => {
            description => "Default extent size (e.g. '1M', '4M').",
            type        => 'string',
            default     => '1M',
        },
        compression => {
            description => "Compression algorithm: 'none', 'lz4', 'zstd'.",
            type        => 'string',
            enum        => ['none', 'lz4', 'zstd'],
            default     => 'none',
        },
        cluster_secret => {
            description => "64 hex-char (32-byte) cluster secret for membership "
                         . "auth and key-store encryption. Prefer 'secret_file' "
                         . "to avoid storing it in plaintext in storage.cfg.",
            type        => 'string',
        },
        secret_file => {
            description => "Path to a 0600 file whose first line is the 64 "
                         . "hex-char cluster secret. Takes precedence over "
                         . "'cluster_secret'.",
            type        => 'string',
        },
        degraded => {
            description => "Allow a clustered mount without SCSI-3 PR fencing "
                         . "(zombie-node risk). Lab use only.",
            type        => 'boolean',
            default     => 0,
        },
    };
}

sub options {
    return {
        path        => { fixed => 1 },
        device      => { optional => 1 },
        maxnodes    => { optional => 1 },
        thin        => { optional => 1 },
        extentsize  => { optional => 1 },
        compression => { optional => 1 },
        cluster_secret => { optional => 1 },
        secret_file => { optional => 1 },
        degraded    => { optional => 1 },
        content     => { optional => 1 },
        nodes       => { optional => 1 },
        shared      => { optional => 1 },
        disable     => { optional => 1 },
        'prune-backups' => { optional => 1 },
        'max-protected-backups' => { optional => 1 },
    };
}

# ═══════════════════════════════════════════════════════════════
# DIRECTORY LAYOUT
#
# Standard PVE layout under the mount point:
#   /mnt/pve/<name>/
#   ├── images/<vmid>/        VM disk images
#   ├── template/
#   │   ├── iso/              ISO images
#   │   └── cache/            Container templates
#   ├── dump/                 VZDump backups
#   ├── rootdir/              Container root filesystems
#   └── snippets/             Cloud-init, hookscripts
# ═══════════════════════════════════════════════════════════════

my $OCSFS_TOOL  = '/usr/sbin/ocsfs-tool';
my $MOUNT_OCSFS = '/usr/sbin/mount.ocsfs';

sub get_subdir {
    my ($class, $scfg, $vtype) = @_;

    my $path = $scfg->{path};

    if ($vtype eq 'images') {
        return "$path/images";
    } elsif ($vtype eq 'iso') {
        return "$path/template/iso";
    } elsif ($vtype eq 'vztmpl') {
        return "$path/template/cache";
    } elsif ($vtype eq 'backup') {
        return "$path/dump";
    } elsif ($vtype eq 'rootdir') {
        return "$path/rootdir";
    } elsif ($vtype eq 'snippets') {
        return "$path/snippets";
    }

    die "unknown vtype '$vtype'\n";
}

# ═══════════════════════════════════════════════════════════════
# MOUNT / UNMOUNT
# ═══════════════════════════════════════════════════════════════

sub _is_mounted {
    my ($path) = @_;

    my $mounts = file_read_firstline('/proc/self/mountinfo') // '';
    open(my $fh, '<', '/proc/self/mountinfo') or return 0;

    while (my $line = <$fh>) {
        my @fields = split(/\s+/, $line);
        # field 4 is the mount point
        if (defined $fields[4] && $fields[4] eq $path) {
            # Check if it's ocsfs
            if ($line =~ /ocsfs/) {
                close($fh);
                return 1;
            }
        }
    }

    close($fh);
    return 0;
}

sub activate_storage {
    my ($class, $storeid, $scfg, $cache) = @_;

    my $path = $scfg->{path};

    if (!_is_mounted($path)) {
        my $device = $scfg->{device};

        die "ocsfs storage '$storeid': no device configured\n"
            unless defined $device;
        die "ocsfs storage '$storeid': device '$device' not found\n"
            unless -e $device;

        # Create mount point if needed
        make_path($path) if !-d $path;

        # Assemble mount options. The cluster secret is required for volumes
        # formatted with auth (mkfs -K) and for the encrypted key store; without
        # it such volumes fail to mount. Prefer secret_file (0600) over the
        # inline cluster_secret so the secret is not kept in storage.cfg.
        my @opts;

        my $secret = $scfg->{cluster_secret};
        if (defined $scfg->{secret_file}) {
            my $fs = file_read_firstline($scfg->{secret_file});
            $secret = $fs if defined $fs && length $fs;
        }
        if (defined $secret) {
            $secret =~ s/\s+//g;
            push @opts, "cluster_secret=$secret";
        }
        push @opts, 'degraded' if $scfg->{degraded};

        # Mount the OCSFS filesystem
        my $cmd = ['mount', '-t', 'ocsfs'];
        push @$cmd, '-o', join(',', @opts) if @opts;
        push @$cmd, $device, $path;

        run_command($cmd, errmsg => "mount ocsfs on '$path'");
    }

    # Ensure directory structure exists
    for my $vtype (qw(images rootdir)) {
        my $subdir = $class->get_subdir($scfg, $vtype);
        make_path($subdir) if !-d $subdir;
    }

    my $templatedir = "$path/template";
    make_path("$templatedir/iso")   if !-d "$templatedir/iso";
    make_path("$templatedir/cache") if !-d "$templatedir/cache";

    my $dumpdir = "$path/dump";
    make_path($dumpdir) if !-d $dumpdir;

    my $snippetdir = "$path/snippets";
    make_path($snippetdir) if !-d $snippetdir;

    return 1;
}

sub deactivate_storage {
    my ($class, $storeid, $scfg, $cache) = @_;

    my $path = $scfg->{path};

    if (_is_mounted($path)) {
        run_command(['umount', $path],
                    errmsg => "umount ocsfs '$path'");
    }

    return 1;
}

# ═══════════════════════════════════════════════════════════════
# STATUS — df-like information
# ═══════════════════════════════════════════════════════════════

sub status {
    my ($class, $storeid, $scfg, $cache) = @_;

    my $path = $scfg->{path};

    return undef if !_is_mounted($path);

    # Use statvfs via df
    my $total = 0;
    my $avail = 0;
    my $used  = 0;
    my $active = 1;

    eval {
        my $res = '';
        run_command(['df', '-P', '-B1', $path],
                    outfunc => sub { $res .= $_[0] . "\n"; });

        my @lines = split(/\n/, $res);
        if (scalar(@lines) >= 2) {
            my @fields = split(/\s+/, $lines[1]);
            $total = int($fields[1]) if defined $fields[1];
            $used  = int($fields[2]) if defined $fields[2];
            $avail = int($fields[3]) if defined $fields[3];
        }
    };
    if ($@) {
        $active = 0;
    }

    return ($total, $avail, $used, $active);
}

# ═══════════════════════════════════════════════════════════════
# VOLUME MANAGEMENT — create, delete, resize
# ═══════════════════════════════════════════════════════════════

sub parse_volname {
    my ($class, $volname) = @_;

    if ($volname =~ m!^(\d+)/(\S+)$!) {
        my ($vmid, $name) = ($1, $2);
        my $format;

        if ($name =~ /\.qcow2$/) {
            $format = 'qcow2';
        } elsif ($name =~ /\.raw$/) {
            $format = 'raw';
        } else {
            $format = 'raw';
        }

        return ('images', $name, $vmid, undef, undef, undef, $format);
    } elsif ($volname =~ m!^iso/(.+)$!) {
        return ('iso', $1);
    } elsif ($volname =~ m!^vztmpl/(.+)$!) {
        return ('vztmpl', $1);
    } elsif ($volname =~ m!^backup/(.+)$!) {
        return ('backup', $1);
    } elsif ($volname =~ m!^rootdir/(\d+)$!) {
        return ('rootdir', $1, $1);
    } elsif ($volname =~ m!^snippets/(.+)$!) {
        return ('snippets', $1);
    }

    die "unable to parse ocsfs volume name '$volname'\n";
}

sub filesystem_path {
    my ($class, $scfg, $volname, $snapname) = @_;

    my ($vtype, $name, $vmid) = $class->parse_volname($volname);
    my $dir = $class->get_subdir($scfg, $vtype);

    my $path;
    if ($vtype eq 'images') {
        $path = "$dir/$vmid/$name";
    } elsif ($vtype eq 'rootdir') {
        $path = "$dir/$name";
    } else {
        $path = "$dir/$name";
    }

    if (defined $snapname) {
        $path .= "\@$snapname";
    }

    return wantarray ? ($path, $vmid, $vtype) : $path;
}

sub create_base {
    my ($class, $storeid, $scfg, $volname) = @_;

    # Base images are read-only templates (for linked clones)
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    my $dir = $class->get_subdir($scfg, 'images');
    my $imgdir = "$dir/$vmid";

    my $basename = $name;
    $basename =~ s/^vm-(\d+)-disk-(\d+)/base-$1-disk-$2/;

    my $src_path  = "$imgdir/$name";
    my $base_path = "$imgdir/$basename";

    rename($src_path, $base_path) or
        die "rename '$src_path' -> '$base_path': $!\n";

    # Mark as immutable (could use OCSFS_IFLAG_IMMUTABLE in the future)
    chmod(0444, $base_path);

    return "$vmid/$basename";
}

sub clone_image {
    my ($class, $scfg, $storeid, $volname, $vmid, $snap) = @_;

    my ($vtype, $name, $srcvmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    my $dir = $class->get_subdir($scfg, 'images');
    my $srcdir = "$dir/$srcvmid";
    my $dstdir = "$dir/$vmid";

    make_path($dstdir) if !-d $dstdir;

    # Find next disk number
    my $disk_num = _next_disk_num($dstdir, $vmid);
    my $ext = ($fmt eq 'qcow2') ? '.qcow2' : '.raw';
    my $dstname = "vm-$vmid-disk-$disk_num$ext";

    my $src_path = "$srcdir/$name";
    my $dst_path = "$dstdir/$dstname";

    # For raw format, use cp --reflink if supported (CoW snapshot)
    # For qcow2, create a backing file chain
    if ($fmt eq 'raw') {
        run_command(['cp', '--reflink=auto', $src_path, $dst_path],
                    errmsg => "clone image");
    } else {
        run_command(['qemu-img', 'create', '-f', 'qcow2',
                     '-b', $src_path, '-F', $fmt,
                     $dst_path],
                    errmsg => "clone qcow2 image");
    }

    return "$vmid/$dstname";
}

sub alloc_image {
    my ($class, $storeid, $scfg, $vmid, $fmt, $name, $size) = @_;

    my $dir = $class->get_subdir($scfg, 'images');
    my $imgdir = "$dir/$vmid";

    make_path($imgdir) if !-d $imgdir;

    if (!$name) {
        my $disk_num = _next_disk_num($imgdir, $vmid);
        my $ext = ($fmt eq 'qcow2') ? '.qcow2' : '.raw';
        $name = "vm-$vmid-disk-$disk_num$ext";
    }

    my $path = "$imgdir/$name";
    my $size_kb = $size;  # PVE passes size in KiB

    if ($fmt eq 'raw') {
        my $thin = $scfg->{thin} // 1;

        if ($thin) {
            # Thin provisioning: use fallocate with KEEP_SIZE
            # This creates the file with declared size but no blocks
            run_command(['truncate', '-s', "${size_kb}K", $path],
                        errmsg => "create thin raw image");
        } else {
            # Thick provisioning: preallocate all blocks
            run_command(['fallocate', '-l', "${size_kb}K", $path],
                        errmsg => "create thick raw image");
        }
    } elsif ($fmt eq 'qcow2') {
        run_command(['qemu-img', 'create', '-f', 'qcow2',
                     $path, "${size_kb}K"],
                    errmsg => "create qcow2 image");
    } else {
        die "unsupported format '$fmt'\n";
    }

    return "$vmid/$name";
}

sub free_image {
    my ($class, $storeid, $scfg, $volname, $isBase, $format) = @_;

    my ($vtype, $name, $vmid) = $class->parse_volname($volname);
    my $dir = $class->get_subdir($scfg, $vtype);

    my $path;
    if ($vtype eq 'images') {
        $path = "$dir/$vmid/$name";
    } else {
        $path = "$dir/$name";
    }

    if (-f $path) {
        unlink($path) or die "unlink '$path': $!\n";
    }

    # Remove empty vmid directory
    if ($vtype eq 'images') {
        my $vmdir = "$dir/$vmid";
        rmdir($vmdir);  # ignore errors (dir may not be empty)
    }

    return undef;
}

sub volume_resize {
    my ($class, $scfg, $storeid, $volname, $size, $running) = @_;

    my $path = $class->filesystem_path($scfg, $volname);
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    my $size_kb = $size;

    if ($fmt eq 'raw') {
        run_command(['truncate', '-s', "${size_kb}K", $path],
                    errmsg => "resize raw image");
    } elsif ($fmt eq 'qcow2') {
        run_command(['qemu-img', 'resize', $path, "${size_kb}K"],
                    errmsg => "resize qcow2 image");
    }

    return undef;
}

# ═══════════════════════════════════════════════════════════════
# VOLUME LISTING
# ═══════════════════════════════════════════════════════════════

sub list_images {
    my ($class, $storeid, $scfg, $vmid, $vollist, $cache) = @_;

    my $dir = $class->get_subdir($scfg, 'images');
    my @res;

    my @vmids;
    if (defined $vmid) {
        @vmids = ($vmid);
    } else {
        opendir(my $dh, $dir) || return \@res;
        @vmids = grep { /^\d+$/ && -d "$dir/$_" } readdir($dh);
        closedir($dh);
    }

    foreach my $id (sort @vmids) {
        my $vmdir = "$dir/$id";
        opendir(my $dh, $vmdir) || next;

        while (my $fn = readdir($dh)) {
            next if $fn =~ /^\./;
            next unless $fn =~ /^(vm|base)-\d+-disk-\d+(\.(raw|qcow2))?$/;

            my $path = "$vmdir/$fn";
            my @stat = stat($path);
            next unless @stat;

            my $size = $stat[7];  # st_size
            my $fmt = ($fn =~ /\.qcow2$/) ? 'qcow2' : 'raw';

            # For qcow2, get virtual size
            if ($fmt eq 'qcow2') {
                eval {
                    my $info = '';
                    run_command(['qemu-img', 'info', '--output=json', $path],
                                outfunc => sub { $info .= $_[0]; });

                    if ($info =~ /"virtual-size":\s*(\d+)/) {
                        $size = int($1);
                    }
                };
            }

            my $volid = "$storeid:$id/$fn";
            my $used = $stat[12] * 512;  # st_blocks * 512

            push @res, {
                volid  => $volid,
                format => $fmt,
                size   => $size,
                vmid   => int($id),
                used   => $used,
            };
        }

        closedir($dh);
    }

    return \@res;
}

# ═══════════════════════════════════════════════════════════════
# SNAPSHOT SUPPORT
# ═══════════════════════════════════════════════════════════════

sub volume_snapshot {
    my ($class, $scfg, $storeid, $volname, $snap) = @_;

    my $path = $class->filesystem_path($scfg, $volname);
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    if ($fmt eq 'qcow2') {
        run_command(['qemu-img', 'snapshot', '-c', $snap, $path],
                    errmsg => "create snapshot '$snap'");
    } elsif ($fmt eq 'raw') {
        # For raw files on OCSFS, use filesystem-level CoW snapshot
        # via ioctl (when kernel support is available)
        my $snap_path = "$path\@$snap";
        run_command(['cp', '--reflink=auto', $path, $snap_path],
                    errmsg => "create raw snapshot '$snap'");
    }

    return undef;
}

sub volume_snapshot_rollback {
    my ($class, $scfg, $storeid, $volname, $snap) = @_;

    my $path = $class->filesystem_path($scfg, $volname);
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    if ($fmt eq 'qcow2') {
        run_command(['qemu-img', 'snapshot', '-a', $snap, $path],
                    errmsg => "rollback to snapshot '$snap'");
    } elsif ($fmt eq 'raw') {
        my $snap_path = "$path\@$snap";
        die "snapshot '$snap' not found\n" unless -f $snap_path;
        run_command(['cp', '--reflink=auto', $snap_path, $path],
                    errmsg => "rollback raw snapshot '$snap'");
    }

    return undef;
}

sub volume_snapshot_delete {
    my ($class, $scfg, $storeid, $volname, $snap) = @_;

    my $path = $class->filesystem_path($scfg, $volname);
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    if ($fmt eq 'qcow2') {
        run_command(['qemu-img', 'snapshot', '-d', $snap, $path],
                    errmsg => "delete snapshot '$snap'");
    } elsif ($fmt eq 'raw') {
        my $snap_path = "$path\@$snap";
        unlink($snap_path) or die "delete snapshot '$snap': $!\n"
            if -f $snap_path;
    }

    return undef;
}

sub volume_snapshot_list {
    my ($class, $scfg, $storeid, $volname) = @_;

    my $path = $class->filesystem_path($scfg, $volname);
    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    my @snaps;

    if ($fmt eq 'qcow2') {
        eval {
            my $info = '';
            run_command(['qemu-img', 'info', '--output=json', $path],
                        outfunc => sub { $info .= $_[0]; });

            # Parse snapshot list from qemu-img info JSON output
            if ($info =~ /"snapshots"\s*:\s*\[(.*?)\]/s) {
                my $snap_json = $1;
                while ($snap_json =~ /"name"\s*:\s*"([^"]+)"/g) {
                    push @snaps, { name => $1 };
                }
            }
        };
    } elsif ($fmt eq 'raw') {
        # List @snapshot files
        my $dir = dirname($path);
        my $base = basename($path);

        opendir(my $dh, $dir) || return \@snaps;
        while (my $fn = readdir($dh)) {
            if ($fn =~ /^\Q$base\E\@(.+)$/) {
                push @snaps, { name => $1 };
            }
        }
        closedir($dh);
    }

    return \@snaps;
}

# ═══════════════════════════════════════════════════════════════
# VOLUME INFORMATION
# ═══════════════════════════════════════════════════════════════

sub volume_size_info {
    my ($class, $scfg, $storeid, $volname, $timeout) = @_;

    my $path = $class->filesystem_path($scfg, $volname);

    my $size = 0;
    my $used = 0;
    my $format = 'raw';

    if ($path =~ /\.qcow2$/) {
        $format = 'qcow2';
        eval {
            my $info = '';
            run_command(['qemu-img', 'info', '--output=json', $path],
                        outfunc => sub { $info .= $_[0]; },
                        timeout => $timeout);

            if ($info =~ /"virtual-size":\s*(\d+)/) {
                $size = int($1);
            }
            if ($info =~ /"actual-size":\s*(\d+)/) {
                $used = int($1);
            }
        };
    } else {
        my @stat = stat($path);
        if (@stat) {
            $size = $stat[7];
            $used = $stat[12] * 512;
        }
    }

    return wantarray ? ($size, $format, $used, undef) : $size;
}

sub volume_has_feature {
    my ($class, $scfg, $feature, $storeid, $volname, $snapname, $running,
        $opts) = @_;

    my $features = {
        snapshot => { current => 1 },
        clone    => { base => 1, current => 1 },
        copy     => { base => 1, current => 1 },
        rename   => { current => 1 },
    };

    my ($vtype, $name, $vmid, undef, undef, undef, $fmt) =
        $class->parse_volname($volname);

    my $key = defined $snapname ? 'snap' : 'current';
    $key = 'base' if $name =~ /^base-/;

    return 1 if defined($features->{$feature}) &&
                defined($features->{$feature}->{$key});

    return undef;
}

# ═══════════════════════════════════════════════════════════════
# HELPERS
# ═══════════════════════════════════════════════════════════════

sub _next_disk_num {
    my ($vmdir, $vmid) = @_;

    my $max = 0;

    opendir(my $dh, $vmdir) || return 0;
    while (my $fn = readdir($dh)) {
        if ($fn =~ /^(?:vm|base)-\d+-disk-(\d+)/) {
            $max = int($1) if int($1) > $max;
        }
    }
    closedir($dh);

    return $max + 1;
}

# ═══════════════════════════════════════════════════════════════
# CHECK CONNECTION — verify device and mount
# ═══════════════════════════════════════════════════════════════

sub check_connection {
    my ($class, $storeid, $scfg) = @_;

    my $device = $scfg->{device};

    # If no device configured, check if already mounted
    if (!$device) {
        return _is_mounted($scfg->{path});
    }

    # Check if the device exists
    return -e $device ? 1 : 0;
}

1;

__END__

=head1 NAME

PVE::Storage::OCSFSPlugin - OCSFS storage plugin for Proxmox VE

=head1 DESCRIPTION

This plugin provides Proxmox VE storage backend support for OCSFS
(Open Cluster Shared FileSystem). It enables shared SAN storage
accessible from multiple PVE cluster nodes simultaneously.

=head1 CONFIGURATION

Add to /etc/pve/storage.cfg:

    ocsfs: fc-shared
        path /mnt/pve/fc-shared
        device /dev/mapper/mpath-3600508b...
        content images,iso,vztmpl,backup,rootdir,snippets
        maxnodes 16
        thin 1
        shared 1

=head1 SEE ALSO

L<PVE::Storage::Plugin>, L<PVE::Storage>

=cut
