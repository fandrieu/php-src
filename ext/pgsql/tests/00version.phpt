--TEST--
PostgreSQL version
--SKIPIF--
<?php include("skipif.inc"); ?>
--FILE--
<?php
// Get postgresql version for easier debugging.
// Execute run-test.php with --keep-all to get version string in 00version.log or 00version.out
include('config.inc');

$db = pg_connect($conn_str);
var_dump(pg_version($db));
pg_close($db);

echo "OK";
?>
--EXPECTF--
array(3) {
  ["client"]=>
  string(%d) "%s"
  ["protocol"]=>
  int(%d)
  ["server"]=>
  string(%d) "%s"
}
OK
