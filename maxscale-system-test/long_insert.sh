#!/bin/bash

rp=`realpath $0`
export src_dir=`dirname $rp`
export test_dir=`pwd`
export test_name=`basename $rp`
$test_dir/non_native_setup $test_name
export ssl_options="--ssl-cert=$src_dir/ssl-cert/client-cert.pem --ssl-key=$src_dir/ssl-cert/client-key.pem"

IP=$Maxscale_IP

mysql -h $IP -P 4006 -u $node_user -p$node_password $ssl_options < $src_dir/long_insert_sql/test_init.sql

echo "RWSplit router:"
for ((i=0 ; i<1000 ; i++)) ; do
	echo "iteration: $i"
	mysql -h $IP -P 4006 -u $node_user -p$node_password $ssl_options < $src_dir/long_insert_sql/test_query.sql
done

echo "ReadConn router (master):"
for ((i=0 ; i<1000 ; i++)) ; do
        echo "iteration: $i"
        mysql -h $IP -P 4008 -u $node_user -p$node_uassword $ssl_options < $src_dir/long_insert_sql/test_query.sql
done


res=$?

$src_dir/copy_logs.sh long_insert
exit $res
