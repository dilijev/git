#!/bin/sh

test_description='ls-files dumping json'

. ./test-lib.sh

strip_number() {
	for name; do
		echo 's/\("'$name'":\) [0-9]\+/\1 <number>/' >>filter.sed
	done
}

strip_string() {
	for name; do
		echo 's/\("'$name'":\) ".*"/\1 <string>/' >>filter.sed
	done
}

compare_json() {
	git ls-files --debug-json >json &&
	sed -f filter.sed json >filtered &&
	test_cmp "$TEST_DIRECTORY"/t3011/"$1" filtered
}

test_expect_success 'setup' '
	mkdir sub &&
	echo one >one &&
	git add one &&
	echo 2 >sub/two &&
	git add sub/two &&

	echo intent-to-add >ita &&
	git add -N ita &&

	strip_number ctime_sec ctime_nsec mtime_sec mtime_nsec &&
	strip_number device inode uid gid file_offset ext_size &&
	strip_string oid ident
'

test_expect_success 'ls-files --json, main entries' '
	compare_json basic
'

test_done