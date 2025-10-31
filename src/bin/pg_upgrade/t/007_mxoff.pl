
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Math::BigInt;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::AdjustDump;
use PostgreSQL::Test::AdjustUpgrade;
use Test::More;

# This test involves different multitransaction states, similarly to that of
# 002_pg_upgrade.pl.

unless (defined($ENV{oldinstall}))
{
	plan skip_all =>
		'to run test set oldinstall environment variable to the pre 64-bit mxoff cluster';
}

# Temp dir for a dumps.
my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Get NextMultiOffset.
sub next_mxoff
{
	my $node = shift;

	my $pg_controldata_path =
		defined($node->install_path) ?
			$node->install_path . '/bin/pg_controldata' :
			'pg_controldata';
	my ($stdout, $stderr) = run_command([ $pg_controldata_path,
											$node->data_dir ]);
	my @control_data = split("\n", $stdout);
	my $next = undef;

	foreach (@control_data)
	{
		if ($_ =~ /^Latest checkpoint's NextMultiOffset:\s*(.*)$/mg)
		{
			$next = $1;
			last;
		}
	}
	die "NextMultiOffset not found in control file\n"
		unless defined($next);

	return $next;
}

# Consume around 10k of mxoffsets.
sub mxact_eater
{
	my $node = shift;
	my $tbl = 'FOO';

	my ($mxoff1, $mxoff2);

	$mxoff1 = next_mxoff($node);
	$node->start;
	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;");

	my $nclients = 10;
	my $update_every = 75;
	my @connections = ();

	for (0..$nclients)
	{
		my $conn = $node->background_psql('postgres');
		$conn->query_safe("BEGIN");

		push(@connections, $conn);
	}

	for (my $i = 0; $i < 1000; $i++)
	{
		my $conn = $connections[$i % $nclients];

		$conn->query_safe("COMMIT;");
		$conn->query_safe("BEGIN");

		if ($i % $update_every == 0)
		{
			$conn->query_safe(
				"UPDATE ${tbl} SET " .
				"N_UPDATED = N_UPDATED + 1 " .
				"WHERE I = ${i} % 50");
		}
		else
		{
			$conn->query_safe(
				"SELECT * FROM ${tbl} FOR KEY SHARE");
		}
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	$mxoff2 = next_mxoff($node);

	return $mxoff1, $mxoff2;
}

# Consume around 2M of mxoffsets.
sub mxact_huge_eater
{
	my $node = shift;
	my $tbl = 'FOO';

	my ($mxoff1, $mxoff2);

	$mxoff1 = next_mxoff($node);
	$node->start;
	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;");

	my $nclients = 10;
	my $update_every = 95;
	my @connections = ();
	my $timeout = 10 * $PostgreSQL::Test::Utils::timeout_default;

	for (0..$nclients)
	{
		my $conn = $node->background_psql('postgres',
										  timeout => $timeout);
		$conn->query_safe("BEGIN");

		push(@connections, $conn);
	}

	# It's a long process, better to tell about progress.
	my $n_steps = 200_000;
	my $step = int($n_steps / 10);

	diag "start to consume mxoffsets ...\n";
	for (my $i = 0; $i < $n_steps; $i++)
	{
		my $conn = $connections[$i % $nclients];

		$conn->query_safe("COMMIT;");
		$conn->query_safe("BEGIN");

		if ($i % $update_every == 0)
		{
			# Perform some non-key UPDATEs too, to exercise different multixact
			# member statuses.
			$conn->query_safe(
				"UPDATE ${tbl} SET " .
				"N_UPDATED = N_UPDATED + 1 " .
				"WHERE I = ${i} % 50");
		}
		else
		{
			$conn->query_safe(
				"SELECT * FROM ${tbl} " .
				"TABLESAMPLE SYSTEM (85) " .
				"FOR KEY SHARE");
		}

		if ($i % $step == 0)
		{
			my $done = int(($i / $n_steps) * 100);
			diag "$done% done...";
		}
	}

	for my $conn (@connections)
	{
		$conn->quit();
	}

	$node->stop;
	$mxoff2 = next_mxoff($node);

	return $mxoff1, $mxoff2;
}

# Handy pg_resetwal wrapper
sub reset_mxoff
{
	my %args = @_;

	my $node = $args{node};
	my $offset = $args{offset};
	my $multi = $args{multi};

	my $pre_64mxoff = defined($node->install_path);
	my $pg_resetwal_path = $pre_64mxoff ?
							$node->install_path . '/bin/pg_resetwal' :
							'pg_resetwal';
	my $blcksz = sub # Get block size
	{
		my $out = (run_command([ $pg_resetwal_path, '--dry-run',
								 $node->data_dir ]))[0];
		$out =~ /^Database block size: *(\d+)$/m or die;
		return $1;
	}->();

	my @cmd;

	# Reset cluster
	@cmd = ($pg_resetwal_path, '--pgdata' => $node->data_dir);
	if (defined($offset))
	{
		push @cmd, '--multixact-offset' => $offset;
	}
	if (defined($multi))
	{
		push @cmd, "--multixact-ids=$multi,$multi";
	}
	command_ok(\@cmd, 'reset multi/offset');

	my $n_items;
	my $segname;

	# Fill empty pg_multixact segments
	if (defined($offset))
	{
		$n_items = 32 * int($blcksz / 20) * 4;
		$segname = sprintf $pre_64mxoff ? "%04X" : "%015X", ($offset / $n_items);
		$segname = $node->data_dir . "/pg_multixact/members/" . $segname;

		@cmd = ('dd');
		push @cmd, "if=/dev/zero";
		push @cmd, "of=" . $segname;
		push @cmd, "bs=$blcksz";
		push @cmd, "count=32";
		command_ok(\@cmd, 'fill empty multixact-members');
	}

	if (defined($multi))
	{
		$n_items = 32 * int($blcksz / ($pre_64mxoff ? 4 : 8));
		$segname = sprintf "%04X", $multi / $n_items;
		$segname = $node->data_dir . "/pg_multixact/offsets/" . $segname;

		@cmd = ('dd');
		push @cmd, "if=/dev/zero";
		push @cmd, "of=" . $segname;
		push @cmd, "bs=$blcksz";
		push @cmd, "count=32";
		command_ok(\@cmd, 'fill empty multixact-offsets');
	}
}

sub get_dump_for_comparison
{
	my ($node, $db, $file_prefix, $adjust_child_columns) = @_;

	my $dumpfile = $tempdir . '/' . $file_prefix . '.sql';
	my $dump_adjusted = "${dumpfile}_adjusted";

	open(my $dh, '>', $dump_adjusted)
	  || die "could not open $dump_adjusted for writing $!";

	$node->run_log(
		[
			'pg_dump', '--no-sync',
			'--restrict-key' => 'test',
			'-d' => $node->connstr($db),
			'-f' => $dumpfile
		]);

	print $dh adjust_regress_dumpfile(slurp_file($dumpfile),
		$adjust_child_columns);
	close($dh);

	return $dump_adjusted;
}

# Main test workhorse routine.
# Make pg_upgrade, dump data and compare it.
sub run_test_core
{
	my $tag = shift;
	my $oldnode = shift;
	my $newnode = shift;

	command_ok(
		[
			'pg_upgrade', '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir,
			'--old-bindir' => $oldnode->config_data('--bindir'),
			'--new-bindir' => $newnode->config_data('--bindir'),
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $newnode->port,
			$mode,
		],
		'run of pg_upgrade for new instance');
	ok( !-d $newnode->data_dir . "/pg_upgrade_output.d",
		"pg_upgrade_output.d/ removed after pg_upgrade success");

	$oldnode->start;
	my $src_dump =
		get_dump_for_comparison($oldnode, 'postgres',
								"oldnode_${tag}_dump", 0);
	$oldnode->stop;

	$newnode->start;
	my $dst_dump =
		get_dump_for_comparison($newnode, 'postgres',
								"newnode_${tag}_dump", 0);
	$newnode->stop;

	compare_files($src_dump, $dst_dump,
		'dump outputs from original and restored regression databases match');
}

sub to_hex
{
	my $arg = shift;

	$arg = Math::BigInt->new($arg);
	$arg = $arg->as_hex();

	return $arg;
}

#
# Per BUG #18863 and BUG #18865
#
sub test_in_wraparound_state
{
	my $tag = shift;
	my $old = shift;
	my $new = shift;

	my $pre_64mxoff_cluster = defined($old->install_path);

	$old->init(extra => ['-k']);

	reset_mxoff(node => $old, multi => 4294967295, offset => 429496729);

	# Create multi transactions
	$old->start;
	$old->safe_psql('postgres',
	qq(
		CREATE TABLE test_table (id integer NOT NULL PRIMARY KEY, val text);
		INSERT INTO test_table VALUES (1, 'a');
	));

	my $conn1 = $old->background_psql('postgres');
	my $conn2 = $old->background_psql('postgres');

	$conn1->query_safe(qq(
		BEGIN;
		SELECT * FROM test_table WHERE id = 1 FOR SHARE;
	));
	$conn2->query_safe(qq(
		BEGIN;
		SELECT * FROM test_table WHERE id = 1 FOR SHARE;
	));

	$conn1->query_safe(qq(COMMIT;));
	$conn2->query_safe(qq(COMMIT;));

	$conn1->quit;
	$conn2->quit;

	$old->stop;

	$new->init;

	run_test_core($tag, $old, $new);
}

#
# case #0: old cluster in wraparound state
#
test_in_wraparound_state(1,
	PostgreSQL::Test::Cluster->new("oldnode_in_wraparound_1"),
	PostgreSQL::Test::Cluster->new("newnode_in_wraparound_1"));

test_in_wraparound_state(2,
	PostgreSQL::Test::Cluster->new("oldnode_in_wraparound_2",
								   install_path => $ENV{oldinstall}),
	PostgreSQL::Test::Cluster->new("newnode_in_wraparound_2"));

sub test
{
	my %args = @_;

	my $tag = $args{tag};
	my $mxoff = $args{offset};
	my $eater = defined($args{func}) ? $args{func} : \&mxact_eater;

	my $old =
		PostgreSQL::Test::Cluster->new("oldnode${tag}",
									   install_path => $ENV{oldinstall});

	$old->init(extra => ['-k']);

	if (defined($mxoff))
	{
		reset_mxoff(node => $old, offset => $mxoff);
	}

	my ($start_mxoff, $finish_mxoff) = $eater->($old);

	my $new = PostgreSQL::Test::Cluster->new("newnode${tag}");
	$new->init;

	run_test_core($tag, $old, $new);

	$start_mxoff = to_hex($start_mxoff);
	$finish_mxoff = to_hex($finish_mxoff);

	my $next_mxoff = to_hex(next_mxoff($new));

	note ">>> case #${tag}\n" .
		 " oldnode mxoff from ${start_mxoff} to ${finish_mxoff}\n" .
		 " newnode mxoff ${next_mxoff}\n";
}

#
# Tests for a single segment
#
test(tag => 1);
test(tag => 2, offset => 0xFFFF0000);
test(tag => 3, offset => 0xFFFFEC77);

#
# Test for a multiple segments
# mxact_huge_eater will comsume >2M offsets
#
diag "\ntest #4 for multiple mxoff segments";
test(tag => 4, func => \&mxact_huge_eater);

diag "\ntest #5 for multiple mxoff segments";
test(tag => 5, func => \&mxact_huge_eater, offset => 0xFFFF0000);

diag "\ntest #r65 for multiple mxoff segments";
test(tag => 6, func => \&mxact_huge_eater, offset => 0xFFFFFFFF - 1_000_000);

done_testing();
