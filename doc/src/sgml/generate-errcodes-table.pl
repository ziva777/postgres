#!/usr/bin/perl
#
# Generate the errcodes-table.sgml file from errcodes.txt
# Copyright (c) 2000-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

print
  "<!-- autogenerated from src/backend/utils/errcodes.txt, do not edit -->\n";

open my $errcodes, '<', $ARGV[0] or die;

while (<$errcodes>)
{
	chomp;

	# Skip comments
	next if /^#/;
	next if /^\s*$/;

	# Emit section headers
	if (/^Section:/)
	{

		# Remove the Section: string
		s/^Section: //;

		# Escape dashes for SGML
		s/-/&mdash;/;

		# Wrap PostgreSQL in <productname/>
		s/PostgreSQL/<productname>PostgreSQL<\/productname>/g;

		print "\n\n";
		print "<row>\n";
		print "<entry spanname=\"span12\">";
		print "<emphasis role=\"bold\">$_</emphasis></entry>\n";
		print "</row>\n";

		next;
	}

	die unless /^([^\s]{5})\s+([EWS])\s+([^\s]+)(?:\s+)?([^\s]+)?/;

	(my $sqlstate, my $type, my $errcode_macro, my $condition_name) =
	  ($1, $2, $3, $4);

	# Skip lines without PL/pgSQL condition names
	next unless defined($condition_name);

	print "\n";
	print "<row>\n";
	print "<entry><literal>$sqlstate</literal></entry>\n";
	print "<entry><symbol>$condition_name</symbol></entry>\n";
	print "</row>\n";
}

close $errcodes;
