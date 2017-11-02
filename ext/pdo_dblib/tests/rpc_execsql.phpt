--TEST--
PDO_DBLIB: RPC Support
--SKIPIF--
<?php
if (!extension_loaded('pdo_dblib')) die('skip not loaded');
require dirname(__FILE__) . '/config.inc';
?>
--FILE--
<?php
require dirname(__FILE__) . '/config.inc';
$db->query('set language english');

/* default = emulation */
var_dump($db->getAttribute(PDO::ATTR_EMULATE_PREPARES));
$st = $db->prepare('select a=:a, b=:b');
$st->execute(['a' => 1, 'b' => 'b']);
var_dump($st->fetch(PDO::FETCH_ASSOC));


/* execsql */
$db->setAttribute(PDO::ATTR_EMULATE_PREPARES, 0);
var_dump($db->getAttribute(PDO::ATTR_EMULATE_PREPARES));
$st = $db->prepare('select a=:a, b=:b');
$st->execute(['a' => 1, 'b' => 'b']);
var_dump($st->fetch(PDO::FETCH_ASSOC));
var_dump($st->getAttribute(PDO::DBLIB_ATTR_RPC));

$st->bindValue('b', 10, PDO::PARAM_INT);
$st->execute();
var_dump($st->fetch(PDO::FETCH_ASSOC));

$st = $db->prepare('select n1=?,n2=?');
$st->execute(['test', 2]);
var_dump($st->fetch(PDO::FETCH_ASSOC));

$st = $db->prepare('set :b=:a');
$st->bindValue('a', "test");
$st->bindParam('b', $b, PDO::PARAM_INPUT_OUTPUT);
$st->execute();
var_dump($b);
?>
--EXPECT--
bool(true)
array(2) {
  ["a"]=>
  string(1) "1"
  ["b"]=>
  string(1) "b"
}
bool(false)
array(2) {
  ["a"]=>
  string(1) "1"
  ["b"]=>
  string(1) "b"
}
bool(true)
array(2) {
  ["a"]=>
  string(1) "1"
  ["b"]=>
  int(10)
}
array(2) {
  ["n1"]=>
  string(4) "test"
  ["n2"]=>
  string(1) "2"
}
string(4) "test"
