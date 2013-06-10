#!/bin/sh

. ../../dttools/src/test_runner.common.sh

chirp_server="../../chirp/src/chirp_server"
psearch="../src/parrot_run ../src/parrot_search"
expected=expected.txt

cpid=cpid.txt
debug=debug.txt
out=output.txt
port=port.txt

prepare()
{
    $chirp_server -r ./fixtures/a -I 127.0.0.1 -Z $port -b -B $cpid -d all -o $debug
    make -C ../src
    exit 0
}

run()
{
    $psearch /chirp/localhost:`cat $port`/ bar > $out
    $psearch /chirp/localhost:`cat $port`/ /bar >> $out
    $psearch /chirp/localhost:`cat $port`/ c/bar >> $out
    $psearch /chirp/localhost:`cat $port`/ /c/bar >> $out
    $psearch /chirp/localhost:`cat $port`/ b/bar >> $out
    $psearch /chirp/localhost:`cat $port`/ /b/bar >> $out
    $psearch /chirp/localhost:`cat $port`/ b/foo >> $out
    $psearch /chirp/localhost:`cat $port`/ /b/foo >> $out
    $psearch /chirp/localhost:`cat $port`/ "/*/foo" >> $out
    $psearch /chirp/localhost:`cat $port`/ "*/*r" >> $out

    noerr=`cat $out | grep -v error`
    rm -f $out
    echo "$noerr" > $out

    failures=`diff --ignore-all-space $out $expected`
    if [ -z "$failures" ]; then
        echo "all tests passed"
        exit 0
    else
        echo $failures
        exit 1
    fi
}

clean()
{
    kill -9 `cat $cpid`
    rm -f $out $cpid $port $debug
    exit 0
}

dispatch $@