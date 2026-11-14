#!/usr/bin/env perl

use strict;
use warnings;

use File::Find;
use File::Basename qw(dirname);
use File::Path qw(make_path);

my $SRC_DIR = "src";
my $BUILD   = "build";
my $OUTPUT  = "$BUILD/libmorgana.so";

my @CFLAGS = (
    "-Wall",
    "-Wextra",
    "-ggdb",
    "-fPIC",
    "-I.",
    "-Iinclude/",
);

my @LDFLAGS = (
    "-shared",
    "-L$ENV{HOME}/.carla/bin/",
    "-leva",
);

sub run {
    my (@cmd) = @_;

    system(@cmd);

    if( $? == -1 ) {
        die "Failed to execute $cmd[0]: $!\n";
    }

    if( $? & 127 ) {
        die "$cmd[0] terminated by signal.\n";
    }

    my $status = $? >> 8;

    die "Command failed with exit code $status.\n"
        if $status != 0;
}

my @sources;

find(
    {
        wanted => sub {
            push @sources, $File::Find::name
                if -f $_ && /\.c$/;
        },
        no_chdir => 1,
    },
    $SRC_DIR
);

@sources = sort @sources;

make_path($BUILD);

my @objects;
my @recompiled;
my $needs_link = 0;

my %existing = map { $_ => 1 } @sources;

foreach my $source (@sources) {
    my $relative = $source;
    $relative =~ s{^\Q$SRC_DIR\E/}{};

    my $object = "$BUILD/$relative";
    $object =~ s/\.c$/.o/;

    my $depfile = $object;
    $depfile =~ s/\.o$/.d/;

    make_path(dirname($object));

    push @objects, $object;

    my $changed = !-f $object || !-f $depfile;

    if(! $changed ) {
        $changed = (stat($source))[9] > (stat($object))[9];
    }

    if(! $changed ) {
        open my $fh, "<", $depfile
            or die "Cannot read $depfile: $!\n";

        local $/;
        my $content = <$fh>;
        close $fh;

        $content =~ s/\\\n/ /g;
        $content =~ s/^[^:]*:\s*//s;

        my @deps = $content =~ /(?:\\.|[^\s])+/g;

        for my $dep (@deps) {
            $dep =~ s/\\(.)/$1/g;

            next unless -f $dep;

            if( (stat($dep))[9] > (stat($object))[9] ) {
                $changed = 1;
                last;
            }
        }
    }

    if( $changed ) {
        run(
            "gcc",
            @CFLAGS,
            "-MMD",
            "-MP",
            "-MF", $depfile,
            "-MT", $object,
            "-c", $source,
            "-o", $object,
        );

        push @recompiled, $source;
        $needs_link = 1;
    }
}

my $old_sources = "$BUILD/sources.out";

if( -f $old_sources ) {
    open my $fh, "<", $old_sources
        or die "Cannot read $old_sources: $!\n";

    while(my $old = <$fh>) {
        chomp $old;

        next if $existing{$old};

        my $relative = $old;
        $relative =~ s{^\Q$SRC_DIR\E/}{};

        my $object = "$BUILD/$relative";
        $object =~ s/\.c$/.o/;

        my $depfile = $object;
        $depfile =~ s/\.o$/.d/;

        unlink $object if -f $object;
        unlink $depfile if -f $depfile;

        $needs_link = 1;
    }

    close $fh;
}

$needs_link = 1 unless -f $OUTPUT;

if(! $needs_link ) {
    for my $object (@objects) {
        next unless -f $object;

        if ((stat($object))[9] > (stat($OUTPUT))[9]) {
            $needs_link = 1;
            last;
        }
    }
}

if ($needs_link) {
    my @valid_objects = grep { -f $_ } @objects;

    die "No object files available for linking.\n"
        unless @valid_objects;

    my $tmp = "$OUTPUT.tmp.$$";

    run(
        "gcc",
        @LDFLAGS,
        "-o", $tmp,
        @valid_objects,
    );

    rename($tmp, $OUTPUT)
        or die "Cannot replace $OUTPUT: $!\n";
}

open my $out, ">", $old_sources
    or die "Cannot write $old_sources: $!\n";

print $out "$_\n" for @sources;

close $out
    or die "Cannot close $old_sources: $!\n";

if( @recompiled || $needs_link ) {
    print "Build completed successfully.\n";
} else {
    print "Everything is up to date.\n";
}
