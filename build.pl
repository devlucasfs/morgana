#!/usr/bin/env perl

use strict;
use warnings;

use File::Find;
use File::Basename qw(dirname);
use File::Path qw(make_path);

my $SRC_DIR     = "src";
my $BUILD       = "build";
my $BIN         = "bin";
my $SCRIPTS_DIR = "scripts";
my $OUTPUT      = "$BIN/libmorgana.so";

my $VERBOSE = 0;
my $ZED_SET_CONFIG = 0;
my $FORCE = 0;

@ARGV = grep {
    if( $_ eq "-v" || $_ eq "--verbose" ) {
        $VERBOSE = 1;
        0;
    } elsif( $_ eq "--force" ) {
        $FORCE = 1;
        0;
    } elsif( $_ eq "--zed-set-config" ) {
        $ZED_SET_CONFIG = 1;
        0;
    } else {
        1;
    }
} @ARGV;

my $MODE = shift // "cpp";

my %MODES = (
    c => {
        extension => qr/\.c$/,
        compiler  => "gcc",
        flags     => ["-ggdb", "-fPIC", "-I.", "-Isrc/"],
        linker    => "gcc",
        linkflags => ["-shared", "-L$ENV{HOME}/.carla/bin/", "-leva"],
        headers   => 0,
    },

    cpp => {
        extension => qr/\.cpp$/,
        compiler  => "g++",
        flags     => ["-ggdb", "-fPIC", "-I.", "-Isrc/"],
        linker    => "g++",
        linkflags => ["-shared", "-L$ENV{HOME}/.carla/bin/", "-leva"],
        headers   => 0,
    },

    "cpp-no-include" => {
        extension => qr/\.cpp$/,
        compiler  => "g++",
        flags     => ["-ggdb", "-fPIC", "-Isrc/", "-I."],
        linker    => "g++",
        linkflags => ["-shared", "-L$ENV{HOME}/.carla/bin/", "-leva"],
        headers   => 1,
    },
);

my @BEAR_CMD = ("bear", "--");

sub verbose {
    my (@args) = @_;

    return unless $VERBOSE;

    print @args;
}

sub run {
    my (@cmd) = @_;

    verbose("[CMD] ", join(" ", @cmd), "\n");

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

sub zed_set_config {
    verbose("\n[ZED] Generating compile_commands.json with bear\n");

    my @inner = ($^X, $0, "--force", $MODE);
    push @inner, "-v" if $VERBOSE;

    run(@BEAR_CMD, @inner);

    print "Zed configs created (compile_commands.json).\n";
}

sub run_prebuild {
    return unless -d $SCRIPTS_DIR;

    my $is_windows = ($^O eq "MSWin32");

    my $script = $is_windows
        ? "$SCRIPTS_DIR/prebuild.cmd"
        : "$SCRIPTS_DIR/prebuild.sh";

    unless( -f $script ) {
        verbose("[PREBUILD] No $script found, skipping.\n");
        return;
    }

    verbose("\n[PREBUILD] Running $script\n");

    if( $is_windows ) {
        (my $win_path = $script) =~ s{/}{\\}g;
        run("cmd", "/c", $win_path);
    } else {
        run("sh", $script);
    }

    verbose("[PREBUILD] Done.\n");
}

sub find_files {
    my ($extension) = @_;
    my @files;

    find(
        {
            wanted => sub {
                push @files, $File::Find::name
                    if -f $_ && $_ =~ $extension;
            },
            no_chdir => 1,
        },
        $SRC_DIR
    );

    return sort @files;
}

sub object_path {
    my ($source) = @_;

    my $relative = $source;
    $relative =~ s{^\Q$SRC_DIR\E/}{};
    $relative =~ s/\.[^.]+$/.o/;

    return "$BUILD/$relative";
}

sub dependencies_changed {
    my ($source, $object, $depfile) = @_;

    return 1 unless -f $object;
    return 1 unless -f $depfile;

    return 1 if(stat($source))[9] > (stat($object))[9];

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

        return 1 if (stat($dep))[9] > (stat($object))[9];
    }

    return 0;
}

die "Source directory '$SRC_DIR' does not exist.\n"
    unless -d $SRC_DIR;

die "Unknown build mode: $MODE\n"
    unless exists $MODES{$MODE};

my $CONFIG = $MODES{$MODE};

if( $ZED_SET_CONFIG ) {
    zed_set_config();
    exit 0;
}

if( $VERBOSE ) {
    print "========================================\n";
    print " Morgana Build Diagnostics\n";
    print "========================================\n";
    print "Mode:       $MODE\n";
    print "Source:     $SRC_DIR\n";
    print "Build:      $BUILD\n";
    print "Output:     $OUTPUT\n";
    print "Compiler:   $CONFIG->{compiler}\n";
    print "Linker:     $CONFIG->{linker}\n";
    print "Extension:  $CONFIG->{extension}\n";
    print "Flags:      ", join(" ", @{$CONFIG->{flags}}), "\n";
    print "========================================\n";
}

run_prebuild();

my @sources = find_files($CONFIG->{extension});
my @headers = $CONFIG->{headers} ? find_files(qr/\.hpp$/) : ();

verbose("\n[DISCOVERY]\n");
verbose("Sources found: ", scalar(@sources), "\n");

for my $source (@sources) {
    verbose("  [SOURCE] $source\n");
}

verbose("Headers found: ", scalar(@headers), "\n");

for my $header (@headers) {
    verbose("  [HEADER] $header\n");
}

if(! @sources ) {
    die "No source files found!\n"
        . "Check the source extension and directory.\n";
}

make_path($BUILD);
make_path($BIN);

my @objects;
my @recompiled;
my $needs_link = 0;

my %existing = map { $_ => 1 } @sources;

my $old_headers = "$BUILD/headers.out";
my %header_changed;

if( $CONFIG->{headers} ) {
    my %old;

    if( -f $old_headers ) {
        open my $fh, "<", $old_headers
            or die "Cannot read $old_headers: $!\n";

        while(my $line = <$fh>) {
            chomp $line;

            my ($path, $mtime) = split /\t/, $line, 2;
            $old{$path} = $mtime;
        }

        close $fh;
    }

    for my $header (@headers) {
        my $mtime = (stat($header))[9];

        $header_changed{$header} =
            !exists $old{$header} || $mtime > $old{$header};
    }
}

foreach my $source (@sources) {
    my $object = object_path($source);

    my $depfile = $object;
    $depfile =~ s/\.o$/.d/;

    make_path(dirname($object));

    push @objects, $object;

    my $changed = dependencies_changed(
        $source,
        $object,
        $depfile,
    );

    $changed = 1 if $FORCE;

    if( $CONFIG->{headers} && !$changed ) {
        for my $header (@headers) {
            if($header_changed{$header}) {
                verbose("[HEADER CHANGED] $header\n");
                $changed = 1;
                last;
            }
        }
    }

    if( $changed ) {
        verbose("\n[COMPILE] $source\n");

        run(
            $CONFIG->{compiler},
            @{$CONFIG->{flags}},
            "-MMD",
            "-MP",
            "-MF", $depfile,
            "-MT", $object,
            "-c", $source,
            "-o", $object,
        );

        push @recompiled, $source;
        $needs_link = 1;
    } else {
        verbose("[UP TO DATE] $source\n");
    }
}

my $old_sources = "$BUILD/sources.out";

if( -f $old_sources ) {
    open my $fh, "<", $old_sources
        or die "Cannot read $old_sources: $!\n";

    while(my $old = <$fh>) {
        chomp $old;

        next if $existing{$old};

        verbose("[REMOVED SOURCE] $old\n");

        my $object = object_path($old);

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

        if( (stat($object))[9] > (stat($OUTPUT))[9] ) {
            $needs_link = 1;
            last;
        }
    }
}

if( $needs_link ) {
    verbose("\n[LINK]\n");

    my @valid_objects = grep { -f $_ } @objects;

    verbose("Objects discovered: ", scalar(@objects), "\n");
    verbose("Valid objects:      ", scalar(@valid_objects), "\n");

    for my $object (@objects) {
        verbose("  ", (-f $object ? "[OK] " : "[MISSING] "), "$object\n");
    }

    die "No object files available for linking.\n"
        . "Mode: $MODE\n"
        . "Sources: " . scalar(@sources) . "\n"
        . "Objects: " . scalar(@objects) . "\n"
        unless @valid_objects;

    my $tmp = "$OUTPUT.tmp.$$";

    run(
        $CONFIG->{linker},
        @{$CONFIG->{linkflags}},
        "-o", $tmp,
        @valid_objects,
    );

    rename($tmp, $OUTPUT)
        or die "Cannot replace $OUTPUT: $!\n";

    verbose("[LINKED] $OUTPUT\n");
} else {
    verbose("\n[LINK] Everything is up to date.\n");
}

open my $out, ">", $old_sources
    or die "Cannot write $old_sources: $!\n";

print $out "$_\n" for @sources;

close $out
    or die "Cannot close $old_sources: $!\n";

if( $CONFIG->{headers} ) {
    open my $out, ">", $old_headers
        or die "Cannot write $old_headers: $!\n";

    for my $header (@headers) {
        print $out "$header\t", (stat($header))[9], "\n";
    }

    close $out
        or die "Cannot close $old_headers: $!\n";
}

if( $VERBOSE ) {
    print "\n========================================\n";
    print " Build Summary\n";
    print "========================================\n";
    print "Mode:       $MODE\n";
    print "Sources:    ", scalar(@sources), "\n";
    print "Headers:    ", scalar(@headers), "\n";
    print "Compiled:   ", scalar(@recompiled), "\n";
    print "Output:     $OUTPUT\n";
    print "========================================\n";
}

if( @recompiled || $needs_link ) {
    print "Build completed successfully.\n";
} else {
    print "Everything is up to date.\n";
}
