#!/usr/bin/perl
# vlt_endeject_notify
#
# This script is called after the vault job completes the eject processing for
# the media to be vaulted.
#
# This script will empty the MAP after a vault eject and insert any tapes
# "returning" from the vault into
#
# Netbackup provides 4 parameters to the script when executed from
# a vault profile:
#  1 = $robot_number     - robot number
#  2 = $vault_name       - logical vault name
#  3 = $vault_profile    - profile name
#  4 = $vault_session_id - vault session id

# This script:
#       must be executable by the root user
#       must exit with 0 upon successful completion
#       must exit with a non zero error code upon failure
#
# CAUTION:  writing anything to stdout or stderr will cause problems in the
#           calling program.
#
# This script closes STDOUT and STDERR and opens them as a pipe to the
# mail command.
# Any errors encountered will be in the output that is emailed.
use strict;
use warnings;

my ($robot_number, $vault_name, $vault_profile, $vault_session_id) = @ARGV;

my $mail_to = join ",", qw(
  admin1@example.com
  admin2@example.com
);

my $vtlcmd_binary      = "/usr/bin/vtlcmd";
my $vmquery_binary     = "/usr/openv/volmgr/bin/vmquery";
my $vmupdate_binary    = "/usr/openv/volmgr/bin/vmupdate";
my $robot_type         = "tld"; # Netbackup robot type
my $mhvtl_robot_number = 50;
my $mail_subject       = "Vault Eject for $vault_name";
my $mail_binary        = find_mail_binary();
my $scratch_pool       = "Scratch";

# Open STDOUT and STDERR as pipes to the mail command only if not running in an
# interactive terminal
if (not -t STDIN and not -t STDOUT) {
  close(STDOUT);
  close(STDERR);
  open(STDOUT, "|-", qq{$mail_binary -s "$mail_subject" "$mail_to"} );
  open(STDERR, ">&", "STDOUT");
}

list_map();
open_map();
empty_map();
insert_tapes();
close_map();
inventory_robot();

sub empty_map {
  print "Emptying MAP: ";
  system("$vtlcmd_binary $mhvtl_robot_number empty map");
  if ($?) {
    die "Error emptying MAP: $!";
  }
}

sub list_map {
  print "Current MAP ";
  system("$vtlcmd_binary $mhvtl_robot_number list map");
  if ($?) {
    die "Error listing contents of MAP: $@";
  }
}

sub open_map {
  print "Opening MAP: ";
  system("$vtlcmd_binary $mhvtl_robot_number open map");
  if ($?) {
    die "Error opening MAP: $!";
  }
}

sub close_map {
  print "Closing MAP: ";
  system("$vtlcmd_binary $mhvtl_robot_number close map");
  if ($?) {
    die "Error closing MAP: $!";
  }
}

sub inventory_robot {
  print "Inventorying robot:\n";
  system("$vmupdate_binary -rt $robot_type -rn $robot_number -full -empty_map -use_barcode_rules");
  if ($?) {
    die "Error inventorying robot: $!";
  }
}

sub insert_tapes {
  # Get all tapes that are "outside" the library and have been returned to the
  # scratch volume pool
  my @returned_tapes = 
    map {
      (split /\s+/, $_)[3]
    }
    grep {
     # This regex filters all lines that don't have the 12th field (Volume Pool)
     # as the Scratch Volume Pool
      /^(?:[^\s]+\s+){11}$scratch_pool/
    } `$vmquery_binary -rt NONE -W`;

  if ($?) {
    die "Error running vmquery: $!";
  }

  foreach my $tape_to_insert (@returned_tapes) {
    print "Loading Tape $tape_to_insert into MAP: ";
    system("$vtlcmd_binary $mhvtl_robot_number load map $tape_to_insert");
    if ($?) {
      die "Error inserting tape into MAP: $!";
    }
  }
}

# Taken from the sample netbackup vlt_endeject_notify script
sub find_mail_binary {
  foreach my $mail_command ( qw( mailx Mail mail ) ) {
    foreach my $mail_dir ( qw( /usr/bin /usr/ucb /usr/sbin /bin /sbin ) ) {
      return "$mail_dir/$mail_command" if -e "$mail_dir/$mail_command";
    }
  }

  die "No mail binary found!";
}
