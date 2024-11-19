# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use File::Find qw(find);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# This pair of calls will create significantly more member segments than offset
# segments.
sub prep
{
	my $node = shift;
	my $tbl = shift;

	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl} (I INT PRIMARY KEY, N_UPDATED INT) " .
		"       WITH (AUTOVACUUM_ENABLED=FALSE);" .
		"INSERT INTO ${tbl} SELECT G, 0 FROM GENERATE_SERIES(1, 50) G;");
}

sub fill
{
	my $node = shift;
	my $tbl = shift;

	my $nclients = 50;
	my $update_every = 90;
	my @connections = ();

	for (0..$nclients)
	{
		my $conn = $node->background_psql('postgres');
		$conn->query_safe("BEGIN");

		push(@connections, $conn);
	}

	for (my $i = 0; $i < 20000; $i++)
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
}

# This pair of calls will create more or less the same amount of membsers and
# offsets segments.
sub prep2
{
	my $node = shift;
	my $tbl = shift;

	$node->safe_psql('postgres',
		"CREATE TABLE ${tbl}(BAR INT PRIMARY KEY, BAZ INT); " .
		"CREATE OR REPLACE PROCEDURE MXIDFILLER(N_STEPS INT DEFAULT 1000) " .
		"LANGUAGE PLPGSQL " .
		"AS \$\$ " .
		"BEGIN " .
		"	FOR I IN 1..N_STEPS LOOP " .
		"		UPDATE ${tbl} SET BAZ = RANDOM(1, 1000) " .
		"		WHERE BAR IN (SELECT BAR FROM ${tbl} " .
		"						TABLESAMPLE BERNOULLI(80)); " .
		"		COMMIT; " .
		"	END LOOP; " .
		"END; \$\$; " .
		"INSERT INTO ${tbl} (BAR, BAZ) " .
		"SELECT ID, ID FROM GENERATE_SERIES(1, 1024) ID;");
}

sub fill2
{
	my $node = shift;
	my $tbl = shift;
	my $scale = shift // 1;

	$node->safe_psql('postgres',
		"BEGIN; " .
		"SELECT * FROM ${tbl} FOR KEY SHARE; " .
		"PREPARE TRANSACTION 'A'; " .
		"CALL MXIDFILLER((365 * ${scale})::int); " .
		"COMMIT PREPARED 'A';");
}


# generate around 2 offset segments and 55 member segments
sub mxid_gen1
{
	my $node = shift;
	my $tbl = shift;

	prep($node, $tbl);
	fill($node, $tbl);

	$node->safe_psql('postgres', q(CHECKPOINT));
}

# generate around 10 offset segments and 12 member segments
sub mxid_gen2
{
	my $node = shift;
	my $tbl = shift;
	my $scale = shift // 1;

	prep2($node, $tbl);
	fill2($node, $tbl, $scale);

	$node->safe_psql('postgres', q(CHECKPOINT));
}

# Fetch latest multixact checkpoint values.
sub multi_bounds
{
	my ($node) = @_;
	my $path = $node->config_data('--bindir');
	my ($stdout, $stderr) = run_command([
									$path . '/pg_controldata',
									$node->data_dir
								]);
	my @control_data = split("\n", $stdout);
	my $next = undef;
	my $oldest = undef;
	my $next_offset = undef;

	foreach (@control_data)
	{
		if ($_ =~ /^Latest checkpoint's NextMultiXactId:\s*(.*)$/mg)
		{
			$next = $1;
			print ">>> @ node ". $node->name . ", " . $_ . "\n";
		}

		if ($_ =~ /^Latest checkpoint's oldestMultiXid:\s*(.*)$/mg)
		{
			$oldest = $1;
			print ">>> @ node ". $node->name . ", " . $_ . "\n";
		}

		if ($_ =~ /^Latest checkpoint's NextMultiOffset:\s*(.*)$/mg)
		{
			$next_offset = $1;
			print ">>> @ node ". $node->name . ", " . $_ . "\n";
		}

		if (defined($oldest) && defined($next) && defined($next_offset))
		{
			last;
		}
	}

	die "Latest checkpoint's NextMultiXactId not found in control file!\n"
	unless defined($next);

	die "Latest checkpoint's oldestMultiXid not found in control file!\n"
	unless defined($oldest);

	die "Latest checkpoint's NextMultiOffset not found in control file!\n"
	unless defined($next_offset);

	return ($oldest, $next, $next_offset);
}

# Create node from existing bins.
sub create_new_node
{
	my ($name, %params) = @_;

	create_node(0, @_);
}

# Create node from ENV oldinstall
sub create_old_node
{
	my ($name, %params) = @_;

	if (!defined($ENV{oldinstall}))
	{
		die "oldinstall is not defined";
	}

	create_node(1, @_);
}

sub create_node
{
	my ($install_path_from_env, $name, %params) = @_;
	my $scale = defined $params{scale} ? $params{scale} : 1;
	my $multi = defined $params{multi} ? $params{multi} : undef;
	my $offset = defined $params{offset} ? $params{offset} : undef;

	my $node =
		$install_path_from_env ?
			PostgreSQL::Test::Cluster->new($name,
					install_path => $ENV{oldinstall}) :
			PostgreSQL::Test::Cluster->new($name);

	$node->init(force_initdb => 1,
		extra => [
			$multi ? ('-m', $multi) : (),
			$offset ? ('-o', $offset) : (),
		]);

	# Fixup MOX patch quirk
	if ($multi)
	{
		unlink $node->data_dir . '/pg_multixact/offsets/0000';
	}
	if ($offset)
	{
		unlink $node->data_dir . '/pg_multixact/members/0000';
	}

	$node->append_conf('fsync', 'off');
	$node->append_conf('postgresql.conf', 'max_prepared_transactions = 2');

	$node->start();
	mxid_gen2($node, 'FOO', $scale);
	mxid_gen1($node, 'BAR', $scale);
	$node->restart();
	$node->safe_psql('postgres', q(SELECT * FROM FOO));		# just in case...
	$node->safe_psql('postgres', q(SELECT * FROM BAR));
	$node->safe_psql('postgres', q(CHECKPOINT));
	$node->stop();

	return $node;
}

sub do_upgrade
{
	my ($oldnode, $newnode) = @_;

	command_ok(
		[
			'pg_upgrade', '--no-sync',
			'-d', $oldnode->data_dir,
			'-D', $newnode->data_dir,
			'-b', $oldnode->config_data('--bindir'),
			'-B', $newnode->config_data('--bindir'),
			'-s', $newnode->host,
			'-p', $oldnode->port,
			'-P', $newnode->port,
			'--check'
		],
		'run of pg_upgrade');

	command_ok(
		[
			'pg_upgrade', '--no-sync',
			'-d', $oldnode->data_dir,
			'-D', $newnode->data_dir,
			'-b', $oldnode->config_data('--bindir'),
			'-B', $newnode->config_data('--bindir'),
			'-s', $newnode->host,
			'-p', $oldnode->port,
			'-P', $newnode->port,
			'--copy'
		],
		'run of pg_upgrade');

	$oldnode->start();
	$newnode->start();

	my $oldfoo = $oldnode->safe_psql('postgres', q(SELECT * FROM FOO));
	my $newfoo = $newnode->safe_psql('postgres', q(SELECT * FROM FOO));
	is($oldfoo, $newfoo, "select foo eq");

	my $oldbar = $oldnode->safe_psql('postgres', q(SELECT * FROM BAR));
	my $newbar = $newnode->safe_psql('postgres', q(SELECT * FROM BAR));
	is($oldbar, $newbar, "select bar eq");

	$oldnode->stop();
	$newnode->stop();

	multi_bounds($oldnode);
	multi_bounds($newnode);
}

my @TESTS = (
	# tests without ENV oldinstall
	0, 1, 2, 3, 4, 5, 6,
	# tests with "real" pg_upgrade
	100, 101, 102, 103, 104, 105, 106,
	# self upgrade
	1000,
);

# =============================================================================
# Basic sanity tests on a NEW bin
# =============================================================================

# starts from the zero
SKIP:
{
	my $TEST_NO = 0;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_mo',
						scale => 1);
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value
SKIP:
{
	my $TEST_NO = 1;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_Mo',
						scale => 1.15,
						multi => '0x123400');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# offsets starts from the value
SKIP:
{
	my $TEST_NO = 2;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_mO',
						scale => 1.15,
						offset => '0x432100');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi and offsets starts from the value
SKIP:
{
	my $TEST_NO = 3;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_MO',
						scale => 1.15,
						multi => '0xDEAD00', offset => '0xBEEF00');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value, multi wrap
SKIP:
{
	my $TEST_NO = 4;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_Mo_wrap',
						scale => 1.15,
						multi => '0xFFFF7000');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# offsets starts from the value, offsets wrap
SKIP:
{
	my $TEST_NO = 5;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_mO_wrap',
						scale => 1.15,
						offset => '0xFFFFFC00');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value, offsets starts from the value,
# multi wrap, offsets wrap
SKIP:
{
	my $TEST_NO = 6;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $node = create_new_node('simple_MO_wrap',
						scale => 1.15,
						multi => '0xFFFF7000', offset => '0xFFFFFC00');
	multi_bounds($node);
	ok(1, "TEST $TEST_NO PASSED");
}

# =============================================================================
# pg_upgarde tests
# =============================================================================

# starts from the zero
SKIP:
{
	my $TEST_NO = 100;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'mo';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1);
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value
SKIP:
{
	my $TEST_NO = 101;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'Mo';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						multi => '0x123400');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# offsets starts from the value
SKIP:
{
	my $TEST_NO = 102;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'mO';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						offset => '0x432100');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi and offsets starts from the value
SKIP:
{
	my $TEST_NO = 103;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'MO';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						multi => '0xDEAD00', offset => '0xBEEF00');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value, multi wrap
SKIP:
{
	my $TEST_NO = 104;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'Mo_wrap';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						multi => '0xFFFF7000');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# offsets starts from the value, offsets wrap
SKIP:
{
	my $TEST_NO = 105;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'mO_wrap';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						offset => '0xFFFFFC00');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# multi starts from the value, offsets starts from the value,
# multi wrap, offsets wrap
SKIP:
{
	my $TEST_NO = 106;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'MO_wrap';
	my $oldnode = create_old_node("old_$dbname",
						scale => 1.2,
						multi => '0xFFFF7000', offset => '0xFFFFFC00');
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

# =============================================================================
# Self upgrade
# =============================================================================

# starts from the zero
SKIP:
{
	my $TEST_NO = 1000;
	skip "do not test case $TEST_NO", 1
		unless ( grep( /^$TEST_NO$/, @TESTS ) );

	my $dbname = 'self_upgrade';
	my $oldnode = create_new_node("old_$dbname",
						scale => 1);
	my $newnode = PostgreSQL::Test::Cluster->new("new_$dbname");
	$newnode->init();

	do_upgrade($oldnode, $newnode);
	ok(1, "TEST $TEST_NO PASSED");
}

done_testing();
