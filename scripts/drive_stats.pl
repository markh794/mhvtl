#!/usr/bin/perl -w
#
# To be called from NetBackup volmgr/bin/drive_unmount_notify
# Parses output of "scsi_command -log_dump"
#
# To collect hard & soft errors from drive(s) along with informaiton
# about bytes sent/received by initiator & bytes written/read to/from media
#
# Released under GPL2 license

##############################################################################
## Modules
###############################################################################

use strict;
use POSIX qw(locale_h);

#############################################################################
## Variables
#############################################################################
my $locale = setlocale(LC_CTYPE);
my $line;
my $tape_usage = 0;
my $read_errors = 0;
my $write_errors = 0;
my $last_drive = 0;
my $last_drive_value = 0;

my $write_bytes_initiator;
my $write_bytes_media;
my $read_bytes_initiator;
my $read_bytes_media;

my $total_corrective_read_errors;
my $total_read_retries;

my $total_corrective_write_errors;
my $total_write_retries;

print "\n  =============== \n";
while($line = <>) {
	$line =~ s/^M//g;       # Strip any cr/lf to lf
	chomp($line);
# Print header information
	if ($line =~ /^Inquiry/) {
		print $line . "\n";
	}
	if ($line =~ /path/) {
		print $line . "\n";
	}
	if ($line =~ /Serial_Number/) {
		print $line . "\n\n";
	}

# Search for Write Error log page (0x02)
	if ($line =~ /^\*\*\*Write\s+Error\s+Log\s+Page/) {
		$tape_usage = 0;
		$write_errors = 1;
		$read_errors = 0;
	}
# Search for Read Error log page (0x03)
	if ($line =~ /^\*\*\*Read\s+Error\s+Log\s+Page/) {
		$tape_usage = 0;
		$write_errors = 0;
		$read_errors = 1;
	}
# Search for Tape Usage log page (0x0C)
	if ($line =~ /^\*\*\*Log\s+page\s+\((.*)\)/) {
		if ($1 =~ /0x0c/) { # Found Tape Usage log page
			$tape_usage = 1;
			$write_errors = 0;
			$read_errors = 0;
		} else { # Not a log page we're interested in - reset all
			$tape_usage = 0;
			$write_errors = 0;
			$read_errors = 0;
		}
	}
	if ($last_drive) { # last loop make key.. This time around is the value
		$last_drive_value = 1;
	}
	if ($line =~ /attribute\s0x020[abcd]/) { # Key match
		$last_drive = 1;
		$last_drive_value = 0;
	}

	if ($tape_usage) {
		if ($line =~ /parameter\s+code:\s+0x0000,\s+value:\s+(.*)/) {
			$write_bytes_initiator = hex("$1");
			print "Bytes written by initiator    : $write_bytes_initiator\n";
		}
		if ($line =~ /parameter\s+code:\s+0x0001,\s+value:\s+(.*)/) {
			$write_bytes_media = hex("$1");
			print "Bytes written to media        : $write_bytes_media\n";
		}
		if ($line =~ /parameter\s+code:\s+0x0002,\s+value:\s+(.*)/) {
			$read_bytes_media = hex("$1");
			print "Bytes read from media         : $read_bytes_media\n";
		}
		if ($line =~ /parameter\s+code:\s+0x0003,\s+value:\s+(.*)/) {
			$read_bytes_initiator = hex("$1");
			print "Bytes read by initiator       : $read_bytes_initiator\n";
		}
	}
	if ($read_errors) {
		if ($line =~ /parameter\s+code:\s+0x0003,\s+value:\s+(.*)/) {
			$total_corrective_read_errors = hex("$1");
			print "Total Corrective read errors  : $total_corrective_read_errors\n";
		}
		if ($line =~ /parameter\s+code:\s+0x0004,\s+value:\s+(.*)/) {
			$total_read_retries = hex("$1");
			print "Total read retries            : $total_read_retries\n";
		}
	}
	if ($write_errors) {
		if ($line =~ /parameter\s+code:\s+0x0003,\s+value:\s+(.*)/) {
			$total_corrective_write_errors = hex("$1");
			print "Total Corrective write errors : $total_corrective_write_errors\n";
		}
		if ($line =~ /parameter\s+code:\s+0x0004,\s+value:\s+(.*)/) {
			$total_write_retries = hex("$1");
			print "Total write retries           : $total_write_retries\n";
		}
	}
	if ($last_drive_value) {
		print "Media previously mounted in  : $line\n";
		$last_drive_value = 0;
		$last_drive = 0;
	}
}

