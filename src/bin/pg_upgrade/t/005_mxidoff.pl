# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use File::Find qw(find);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!defined($ENV{oldinstall}))
{
	die "oldinstall is not defined";
}

sub mxid_prepare
{
	my ($node) = @_;

	$node->safe_psql('postgres',
	q(
	CREATE TABLE FOO(BAR INT PRIMARY KEY, BAZ INT);
	CREATE OR REPLACE PROCEDURE MXIDFILLER(N_STEPS INT DEFAULT 1000)
	LANGUAGE PLPGSQL
	AS $$
	BEGIN
		FOR I IN 1..N_STEPS LOOP
			UPDATE FOO SET BAZ = RANDOM(1, 1000)
			WHERE BAR IN (SELECT BAR FROM FOO TABLESAMPLE BERNOULLI(80));
			COMMIT;
		END LOOP;
	END;$$;
	INSERT INTO FOO (BAR, BAZ) SELECT ID, ID FROM GENERATE_SERIES(1, 512) ID;
	));
}

sub mxid_fill
{
	my ($node) = @_;

	$node->safe_psql('postgres',
	q(
	BEGIN;
	SELECT * FROM FOO FOR KEY SHARE;
	PREPARE TRANSACTION 'A';
	CALL MXIDFILLER(365);
	COMMIT PREPARED 'A';
	),
	timeout => 3600);
}

# Fetch latest multixact checkpoint values.
sub multi_bounds
{
	my ($node) = @_;
	my ($stdout, $stderr) = run_command([ 'pg_controldata', $node->data_dir ]);
	my @control_data = split("\n", $stdout);
	my $next = undef;
	my $oldest = undef;

	foreach (@control_data)
	{
		if ($_ =~ /^Latest checkpoint's NextMultiXactId:\s*(.*)$/mg)
		{
			$next = $1;
		}

		if ($_ =~ /^Latest checkpoint's oldestMultiXid:\s*(.*)$/mg)
		{
			$oldest = $1;
		}

		if (defined($oldest) && defined($next))
		{
			last;
		}
	}

	die "Latest checkpoint's NextMultiXactId not found in control file!\n"
	unless defined($next);

	die "Latest checkpoint's oldestMultiXid not found in control file!\n"
	unless defined($oldest);

	return ($oldest, $next);
}

# List pg_multixact/offsets segments filenames.
sub list_actual_multixact_offsets
{
	my ($node) = @_;
	my $dir;

	opendir($dir, $node->data_dir . '/pg_multixact/offsets') or die $!;
	my @list = sort grep { /[0-9A-F]+/ } readdir $dir;
	closedir $dir;

	return @list;
}

use constant SIZEOF_MULTI_XACT_OFFSET   => 8;
use constant BLCKSZ                     => 8192;
use constant MULTIXACT_OFFSETS_PER_PAGE => BLCKSZ / SIZEOF_MULTI_XACT_OFFSET;
use constant SLRU_PAGES_PER_SEGMENT     => 2;

# See src/backend/access/transam/multixact.c
sub MultiXactIdToOffsetSegment
{
	my ($multi) = @_;

	return $multi / MULTIXACT_OFFSETS_PER_PAGE / SLRU_PAGES_PER_SEGMENT;
}

# Validate pg_multixact/offsets segments conversion.
sub validate_multixact_offsets
{
	my ($old, $new, $oldnode) = @_;
	my ($oldest, $next) = multi_bounds($oldnode);
	my $maxsegno = MultiXactIdToOffsetSegment($next);
	my $maxsegname = sprintf("%04X", $maxsegno);

	print(">>>>>>>>>\n");
	foreach my $segname ( @$old )
	{
		my $segno = hex($segname) * 2;
		my $converted1 = sprintf("%04X", $segno);
		my $converted2 = sprintf("%04X", $segno + 1);

		print "[${segname}] -> [${converted1}, ${converted2}] \n";
		# Skip the last segment as it may be incomplete.
		if (not $converted1 eq $maxsegname)
		{
			die "Segmanet ${segname} is not properly converted"
			unless (not $converted1 eq $maxsegname) and
				   grep { $converted1 eq $_ } @$new and
				   grep { $converted2 eq $_ } @$new;
		}
	}
	print(">>>>>>>>>\n");

	return 1;
}

#
# Select tests to run.
#
my @tests = (0, 1, 2, 3, 4);

# =============================================================================
# CASE 0
#
# There must be several segments starting from the zero.
# =============================================================================
SKIP:
{
	skip "case 0", 0
		unless ( grep( /^0$/, @tests ) );

	my $oldnode = PostgreSQL::Test::Cluster->new('old_node0',
											  install_path => $ENV{oldinstall});
	$oldnode->init(force_initdb => 1);
	$oldnode->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
	$oldnode->append_conf('fsync', 'off');

	my $newnode = PostgreSQL::Test::Cluster->new('new_node0');
	$newnode->init();

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

	my @o = list_actual_multixact_offsets($oldnode);
	my @n = list_actual_multixact_offsets($newnode);
	ok(validate_multixact_offsets(\@o, \@n, $oldnode),
		"case0: offsets segmants matched");

	$oldnode->start();
	$newnode->start();

	# just in case...
	my $oldval = $oldnode->safe_psql('postgres', q(SELECT 1));
	my $newval = $newnode->safe_psql('postgres', q(SELECT 1));
	is($oldval, $newval, "case1: select eq");

	$oldnode->stop();
	$newnode->stop();
}

# =============================================================================
# CASE 1
#
# There must be several segments starting from the zero.
# =============================================================================
SKIP:
{
	skip "case 1", 1
		unless ( grep( /^1$/, @tests ) );

	my $oldnode = PostgreSQL::Test::Cluster->new('old_node1',
											  install_path => $ENV{oldinstall});
	$oldnode->init(force_initdb => 1);
	$oldnode->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
	$oldnode->append_conf('fsync', 'off');
	$oldnode->start();

	mxid_prepare($oldnode);
	mxid_fill($oldnode);

	$oldnode->safe_psql('postgres', q(CHECKPOINT));
	$oldnode->stop();

	my $newnode = PostgreSQL::Test::Cluster->new('new_node1');
	$newnode->init();

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

	my @o = list_actual_multixact_offsets($oldnode);
	my @n = list_actual_multixact_offsets($newnode);
	ok(validate_multixact_offsets(\@o, \@n, $oldnode),
		"case1: offsets segmants matched");

	$oldnode->start();
	$newnode->start();

	# just in case...
	my $oldval = $oldnode->safe_psql('postgres', q(SELECT * FROM FOO));
	my $newval = $newnode->safe_psql('postgres', q(SELECT * FROM FOO));
	is($oldval, $newval, "case1: select eq");

	$oldnode->stop();
	$newnode->stop();
}

# =============================================================================
# CASE 2
#
# Non-standard oldestMultiXid and NextMultiXactId.
# There must be several segments starting from some value.
# =============================================================================
SKIP:
{
	skip "case 2", 2
		unless ( grep( /^2$/, @tests ) );

	my $oldnode = PostgreSQL::Test::Cluster->new('old_node2',
											  install_path => $ENV{oldinstall});
	$oldnode->init(force_initdb => 1,
				extra => [
					'-m', '0x123000', '-o', '0x123000'
				]);

	# Fixup MOX patch quirk
	unlink $oldnode->data_dir . '/pg_multixact/members/0000';
	unlink $oldnode->data_dir . '/pg_multixact/offsets/0000';

	$oldnode->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
	$oldnode->append_conf('fsync', 'off');
	$oldnode->start();

	mxid_prepare($oldnode);
	mxid_fill($oldnode);

	$oldnode->safe_psql('postgres', q(CHECKPOINT));
	$oldnode->stop();

	my $newnode = PostgreSQL::Test::Cluster->new('new_node2');
	$newnode->init();

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

	my @o = list_actual_multixact_offsets($oldnode);
	my @n = list_actual_multixact_offsets($newnode);
	ok(validate_multixact_offsets(\@o, \@n, $oldnode),
		"case2: non-standard offsets segmants matched");

	$oldnode->start();
	$newnode->start();

	# just in case...
	my $oldval = $oldnode->safe_psql('postgres', q(SELECT * FROM FOO));
	my $newval = $newnode->safe_psql('postgres', q(SELECT * FROM FOO));
	is($oldval, $newval, "case2: select eq");

	$oldnode->stop();
	$newnode->stop();
}

# =============================================================================
# CASE 3
#
# Non-standard oldestMultiXid and NextMultiXactId.
# =============================================================================
SKIP:
{
	skip "case 3", 3
		unless ( grep( /^3$/, @tests ) );
	chdir ${PostgreSQL::Test::Utils::tmp_check};
	my $oldnode = PostgreSQL::Test::Cluster->new('old_node3',
											  install_path => $ENV{oldinstall});
	$oldnode->init(force_initdb => 1,
				extra => [
					'-m', '0xFFFF0000', '-o', '0xFFFF0000'
				]);

	# Fixup MOX patch quirk
	unlink $oldnode->data_dir . '/pg_multixact/members/0000';
	unlink $oldnode->data_dir . '/pg_multixact/offsets/0000';

	$oldnode->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
	$oldnode->append_conf('fsync', 'off');
	$oldnode->start();

	mxid_prepare($oldnode);
	mxid_fill($oldnode);
	mxid_fill($oldnode);

	$oldnode->safe_psql('postgres', q(CHECKPOINT));
	$oldnode->stop();

	my $newnode = PostgreSQL::Test::Cluster->new('new_node3');
	$newnode->init();

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

	my @o = list_actual_multixact_offsets($oldnode);
	my @n = list_actual_multixact_offsets($newnode);
	ok(validate_multixact_offsets(\@o, \@n, $oldnode),
		"case3: multi warp, non-standard offsets segmants matched");

	$oldnode->start();
	$newnode->start();

	# just in case...
	my $oldval = $oldnode->safe_psql('postgres', q(SELECT * FROM FOO));
	my $newval = $newnode->safe_psql('postgres', q(SELECT * FROM FOO));
	is($oldval, $newval, "case3: select eq");

	$oldnode->stop();
	$newnode->stop();
}

# =============================================================================
# CASE 4
#
# Non-standard oldestMultiXid and NextMultiXactId.
# offset segments wraparound
# =============================================================================
SKIP:
{
	skip "case 4", 4
		unless ( grep( /^4$/, @tests ) );
	chdir ${PostgreSQL::Test::Utils::tmp_check};
	my $oldnode = PostgreSQL::Test::Cluster->new('old_node4',
											  install_path => $ENV{oldinstall});
	$oldnode->init(force_initdb => 1,
				extra => [
					'-m', '0xFFFF0000', '-o', '0xFFFF0000'
				]);

	# Fixup MOX patch quirk
	unlink $oldnode->data_dir . '/pg_multixact/members/0000';
	unlink $oldnode->data_dir . '/pg_multixact/offsets/0000';

	$oldnode->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
	$oldnode->append_conf('fsync', 'off');
	$oldnode->start();

	mxid_prepare($oldnode);
	mxid_fill($oldnode);
	mxid_fill($oldnode);
	mxid_fill($oldnode);
	mxid_fill($oldnode);
	mxid_fill($oldnode);

	$oldnode->safe_psql('postgres', q(CHECKPOINT));
	$oldnode->stop();

	my $newnode = PostgreSQL::Test::Cluster->new('new_node4');
	$newnode->init();

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

	my @o = list_actual_multixact_offsets($oldnode);
	my @n = list_actual_multixact_offsets($newnode);
	ok(validate_multixact_offsets(\@o, \@n, $oldnode),
		"case3: multi warp, non-standard offsets segmants matched");

	$oldnode->start();
	$newnode->start();

	# just in case...
	my $oldval = $oldnode->safe_psql('postgres', q(SELECT * FROM FOO));
	my $newval = $newnode->safe_psql('postgres', q(SELECT * FROM FOO));
	is($oldval, $newval, "case4: select eq");

	$oldnode->stop();
	$newnode->stop();
}

done_testing();
