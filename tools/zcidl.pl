#!/usr/bin/perl

use Getopt::Long;
use strict;
use warnings;
our $gen_mode;
&Getopt::Long::Configure(qw(bundling));
&GetOptions(
	"c" => sub { $gen_mode = "CLN"; },
	"d" => sub { $gen_mode = "SDF"; },
);

if ($gen_mode eq "CLN") {
	print "#include <$_>\n" for qw(utility gromox/defs.h gromox/zcore_client.hpp gromox/zcore_rpc.hpp);
	print "#include \"$_\"\n" for qw(php.h);
}

while (<STDIN>) {
	next if (!m{^\s*ZCIDL\(\s*(\w+)\s*,\s*\((.*)\)\)});
	my($func, $iargs, $oargs, $iargf, $oargf) = ($1, $2, [], [], []);
	if ($iargs =~ s{^(.*),\s*IDLOUT\s+(.*)}{$1, $2}) {
		$iargs = $1;
		$oargf = [&split_argl($2)];
		$oargs = [&split_adcl(@$oargf)];
	}
	$iargf = [&split_argl($iargs)];
	$iargs = [&split_adcl(@$iargf)];
	my $rbsig = join(", ", @$iargf, @$oargf);

	if ($gen_mode eq "SDF") {
		print "case zcore_callid::$func: {\n";
		if (scalar(@$iargs) > 0) {
			print "\tauto &q = *static_cast<const zcreq_$func *>(q0);\n";
		}
		print "\tauto r1 = cu_alloc<zcresp_$func>();\n";
		print "\tr0 = r1;\n";
		print "\tif (r1 == nullptr) return false;\n";
		if (scalar(@$oargs) > 0) {
			print "\tauto &r = *r1;\n";
		}
		print "\tr0->result = zs_$func(", join(", ",
			(map { my($type, $field) = @$_; (substr($type, -1, 1) eq "&" ? "*" : "")."q.$field"; } @$iargs),
			(map { my($type, $field) = @$_; (substr($type, -1, 1) eq "&" ? "" : "&")."r.$field"; } @$oargs),
		), ");\n";
		print "\tbreak;\n}\n";
		next;
	}

	print "uint32_t zclient_$func($rbsig)\n{\n";
	print "\tzcreq_$func q{zcore_callid::$func";
	for (@$iargs) {
		my($type, $field) = @$_;
		if (substr($type, -1, 1) eq "*") {
			print ", deconst($field)";
		} elsif (substr($type, -1, 1) eq "&") {
			print ", deconst(&$field)";
		} else {
			print ", $field";
		}
	}
	print "};\n\tzcresp_$func r{};\n\n";
	print "\tif (!zclient_do_rpc(&q, &r))\n\t\treturn ecRpcFailed;\n";
	if (scalar(@$oargs) > 0) {
		print "\tif (r.result != ecSuccess)\n\t\treturn r.result;\n";
	}
	for (@$oargs) {
		my($type, $field) = @$_;
		print "\t", (substr($type, -1, 1) eq "&" ? "" : "*"),
		      "$field = std::move(r.$field);\n";
	}
	print "\treturn r.result;\n}\n\n";
}

sub split_adcl { return map { [&fname($_)] } @_; }
sub split_argl { return map { $_ eq "" ? () : ($_) } split(qr{\s*,\s*}, shift(@_)); }
sub fname
{
	$_[0] =~ /(\s*(\w+)\s*)$/;
	my $type = substr($_[0], 0, -length($1));
	return ($type, $2);
}
