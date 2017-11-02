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
$st = $db->prepare('select a=@a, b=@b');
$st->execute(['a' => 1, 'b' => 'b']);
var_dump($st->fetch(PDO::FETCH_ASSOC));

$st->bindValue('b', 10, PDO::PARAM_INT);
$st->execute();
var_dump($st->fetch(PDO::FETCH_ASSOC));

$st = $db->prepare('select num1=@1');
$st->execute(['test']);
var_dump($st->fetch(PDO::FETCH_ASSOC));
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
array(2) {
  ["a"]=>
  string(1) "1"
  ["b"]=>
  int(10)
}
array(1) {
  ["num1"]=>
  string(4) "test"
}
