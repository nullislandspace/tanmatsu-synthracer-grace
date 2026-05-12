#!/usr/bin/env perl
use strict;
use warnings;
use File::Basename;

# NBT tag types (must match game_nbt.h)
use constant {
    NBT_END      => 0x00,
    NBT_INT32    => 0x01,
    NBT_DOUBLE   => 0x02,
    NBT_STRING   => 0x03,
    NBT_COMPOUND => 0x04,
    NBT_INT64    => 0x05,
};

use constant NBT_ENDIAN_CHECK => 0x01020304;

my %type_name = (
    NBT_END()      => 'END',
    NBT_INT32()    => 'INT32',
    NBT_DOUBLE()   => 'DOUBLE',
    NBT_STRING()   => 'STRING',
    NBT_COMPOUND() => 'COMPOUND',
    NBT_INT64()    => 'INT64',
);

# ---------------------------------------------------------------------------
# Binary reading helpers
# ---------------------------------------------------------------------------
my $swap_endian = 0;

sub read_bytes {
    my ($fh, $n) = @_;
    my $buf;
    my $nread = read($fh, $buf, $n);
    die "Unexpected EOF (wanted $n bytes, got " . ($nread // 0) . ")\n"
        unless defined $nread && $nread == $n;
    return $buf;
}

sub read_u8  { return unpack('C', read_bytes($_[0], 1)); }

sub read_u16 {
    my $raw = read_bytes($_[0], 2);
    my $v = unpack('v', $raw);  # little-endian
    $v = unpack('n', $raw) if $swap_endian;  # big-endian
    return $v;
}

sub read_u32 {
    my $raw = read_bytes($_[0], 4);
    my $v = unpack('V', $raw);
    $v = unpack('N', $raw) if $swap_endian;
    return $v;
}

sub read_i32 {
    my $raw = read_bytes($_[0], 4);
    my $v;
    if ($swap_endian) {
        $v = unpack('l>', $raw);
    } else {
        $v = unpack('l<', $raw);
    }
    return $v;
}

sub read_i64 {
    my $raw = read_bytes($_[0], 8);
    my $v;
    if ($swap_endian) {
        $v = unpack('q>', $raw);
    } else {
        $v = unpack('q<', $raw);
    }
    return $v;
}

sub read_f64 {
    my $raw = read_bytes($_[0], 8);
    if ($swap_endian) {
        $raw = reverse $raw;
    }
    return unpack('d', $raw);
}

sub read_name {
    my ($fh) = @_;
    my $len = read_u16($fh);
    return '' if $len == 0;
    return read_bytes($fh, $len);
}

sub read_string_payload {
    my ($fh) = @_;
    my $len = read_u16($fh);
    return '' if $len == 0;
    return read_bytes($fh, $len);
}

# ---------------------------------------------------------------------------
# Binary writing helpers
# ---------------------------------------------------------------------------

sub write_u8  { print {$_[0]} pack('C', $_[1]); }
sub write_u16 { print {$_[0]} pack('v', $_[1]); }
sub write_u32 { print {$_[0]} pack('V', $_[1]); }

sub write_i32 {
    my ($fh, $v) = @_;
    print {$fh} pack('l<', $v);
}

sub write_i64 {
    my ($fh, $v) = @_;
    print {$fh} pack('q<', $v);
}

sub write_f64 {
    my ($fh, $v) = @_;
    print {$fh} pack('d', $v);
}

sub write_name {
    my ($fh, $name) = @_;
    write_u16($fh, length($name));
    print {$fh} $name;
}

# ---------------------------------------------------------------------------
# Decompile: read NBT file, output indented text
# ---------------------------------------------------------------------------

sub decompile_tag {
    my ($fh, $out, $indent) = @_;
    my $prefix = '  ' x $indent;

    my $type = read_u8($fh);
    return 0 if $type == NBT_END;

    my $name = read_name($fh);

    if ($type == NBT_INT32) {
        my $val = read_i32($fh);
        print $out "${prefix}INT32 \"$name\" = $val\n";
    } elsif ($type == NBT_INT64) {
        my $val = read_i64($fh);
        print $out "${prefix}INT64 \"$name\" = $val\n";
    } elsif ($type == NBT_DOUBLE) {
        my $val = read_f64($fh);
        if ($val == int($val) && abs($val) < 1e15) {
            print $out "${prefix}DOUBLE \"$name\" = " . sprintf("%.1f", $val) . "\n";
        } else {
            print $out "${prefix}DOUBLE \"$name\" = " . sprintf("%.17g", $val) . "\n";
        }
    } elsif ($type == NBT_STRING) {
        my $val = read_string_payload($fh);
        # Escape special characters for display
        $val =~ s/\\/\\\\/g;
        $val =~ s/"/\\"/g;
        $val =~ s/\n/\\n/g;
        $val =~ s/\r/\\r/g;
        print $out "${prefix}STRING \"$name\" = \"$val\"\n";
    } elsif ($type == NBT_COMPOUND) {
        print $out "${prefix}COMPOUND \"$name\" {\n";
        while (decompile_tag($fh, $out, $indent + 1)) { }
        print $out "${prefix}}\n";
    } else {
        die "Unknown tag type $type at offset " . tell($fh) . "\n";
    }

    return 1;
}

sub decompile {
    my ($bin_file, $txt_file) = @_;

    open my $fh, '<:raw', $bin_file or die "Cannot open $bin_file: $!\n";

    # Read header
    my $magic = read_bytes($fh, 4);
    die "Bad magic: expected SYNT, got '$magic'\n" unless $magic eq 'SYNT';

    my $endian_raw = read_u32($fh);
    # Note: we read it as LE. If it's actually BE, the value will be swapped.
    if ($endian_raw == NBT_ENDIAN_CHECK) {
        $swap_endian = 0;
    } elsif ($endian_raw == 0x04030201) {
        $swap_endian = 1;
        warn "File is big-endian, will byte-swap\n";
    } else {
        die sprintf("Bad endian sentinel: 0x%08X\n", $endian_raw);
    }

    my $version = read_u16($fh);

    my $out;
    if ($txt_file eq '-') {
        $out = \*STDOUT;
    } else {
        open $out, '>', $txt_file or die "Cannot open $txt_file for writing: $!\n";
    }

    print $out "# Race the Synth NBT save file\n";
    print $out "# Format version: $version\n";
    print $out "# Endian swap: $swap_endian\n\n";

    # Read tags until EOF
    while (!eof($fh)) {
        last unless decompile_tag($fh, $out, 0);
    }

    close $fh;
    close $out unless $txt_file eq '-';
    warn "Decompiled $bin_file -> $txt_file (version $version)\n" unless $txt_file eq '-';
}

# ---------------------------------------------------------------------------
# Compile: read indented text, write NBT file
# ---------------------------------------------------------------------------

sub compile {
    my ($txt_file, $bin_file) = @_;

    open my $in, '<', $txt_file or die "Cannot open $txt_file: $!\n";
    my @lines;
    my $lineno = 0;
    while (<$in>) {
        $lineno++;
        s/#.*//;       # strip comments
        s/^\s+//;
        s/\s+$//;
        next if /^$/;
        push @lines, { text => $_, lineno => $lineno };
    }
    close $in;

    open my $out, '>:raw', $bin_file or die "Cannot open $bin_file for writing: $!\n";

    # Write header
    print $out 'SYNT';
    write_u32($out, NBT_ENDIAN_CHECK);
    write_u16($out, 1);  # format version

    my $pos = 0;
    compile_tags($out, \@lines, \$pos);

    close $out;
    warn "Compiled $txt_file -> $bin_file\n";
}

sub compile_tags {
    my ($out, $lines, $pos_ref) = @_;

    while ($$pos_ref < scalar @$lines) {
        my $line = $lines->[$$pos_ref];
        my $text = $line->{text};
        my $ln   = $line->{lineno};

        # COMPOUND "name" {
        if ($text =~ /^COMPOUND\s+"([^"]*)"\s*\{?\s*$/) {
            my $name = $1;
            write_u8($out, NBT_COMPOUND);
            write_name($out, $name);
            $$pos_ref++;

            # Recurse for children until we hit a closing brace
            compile_tags($out, $lines, $pos_ref);

            # Write END
            write_u8($out, NBT_END);
            next;
        }

        # Closing brace (end of compound) — return to parent
        if ($text =~ /^\}/) {
            $$pos_ref++;
            return;
        }

        # INT32 "name" = value
        if ($text =~ /^INT32\s+"([^"]*)"\s*=\s*(.+)$/) {
            write_u8($out, NBT_INT32);
            write_name($out, $1);
            write_i32($out, int($2));
            $$pos_ref++;
            next;
        }

        # INT64 "name" = value
        if ($text =~ /^INT64\s+"([^"]*)"\s*=\s*(.+)$/) {
            write_u8($out, NBT_INT64);
            write_name($out, $1);
            write_i64($out, int($2));
            $$pos_ref++;
            next;
        }

        # DOUBLE "name" = value
        if ($text =~ /^DOUBLE\s+"([^"]*)"\s*=\s*(.+)$/) {
            write_u8($out, NBT_DOUBLE);
            write_name($out, $1);
            write_f64($out, $2 + 0.0);
            $$pos_ref++;
            next;
        }

        # STRING "name" = "value"
        if ($text =~ /^STRING\s+"([^"]*)"\s*=\s*"((?:[^"\\]|\\.)*)"$/) {
            my ($name, $val) = ($1, $2);
            # Unescape
            $val =~ s/\\n/\n/g;
            $val =~ s/\\r/\r/g;
            $val =~ s/\\"/"/g;
            $val =~ s/\\\\/\\/g;
            write_u8($out, NBT_STRING);
            write_name($out, $name);
            write_u16($out, length($val));
            print $out $val;
            $$pos_ref++;
            next;
        }

        warn "Line $ln: cannot parse: $text\n";
        $$pos_ref++;
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

sub usage {
    die "Usage: $0 decompile <save.bin> [output.txt]\n" .
        "       $0 compile   <input.txt> <save.bin>\n";
}

my $cmd = shift @ARGV or usage();

if ($cmd eq 'decompile') {
    my $bin = shift @ARGV or usage();
    my $txt = shift @ARGV || '-';
    decompile($bin, $txt);
} elsif ($cmd eq 'compile') {
    my $txt = shift @ARGV or usage();
    my $bin = shift @ARGV or usage();
    compile($txt, $bin);
} else {
    usage();
}
