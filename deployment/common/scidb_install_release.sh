#!/bin/bash
#
# BEGIN_COPYRIGHT
#
# This file is part of SciDB.
# Copyright (C) 2008-2014 SciDB, Inc.
#
# SciDB is free software: you can redistribute it and/or modify
# it under the terms of the AFFERO GNU General Public License as published by
# the Free Software Foundation.
#
# SciDB is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
# INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
# NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
# the AFFERO GNU General Public License for the complete license terms.
#
# You should have received a copy of the AFFERO GNU General Public License
# along with SciDB.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
#
# END_COPYRIGHT
#

function centos6 ()
{
    /sbin/chkconfig iptables off
    /sbin/service iptables stop
    if [ "1" == "${with_coordinator}" ]; then
	yum install --enablerepo=scidb -y scidb-${release}-all-coord scidb-${release}-dev-tools
    else
	yum install --enablerepo=scidb -y scidb-${release}-all
    fi
}

function ubuntu1204 ()
{
    apt-get update
    if [ "1" == "${with_coordinator}" ]; then
	apt-get install -y scidb-${release}-all-coord scidb-${release}-dev-tools
    else
	apt-get install -y scidb-${release}-all
    fi
}

OS=`./os_detect.sh`
release=${1}
with_coordinator=${2}

if [ "${OS}" = "CentOS 6" ]; then
    centos6
fi

if [ "${OS}" = "RedHat 6" ]; then
    centos6
fi

if [ "${OS}" = "Ubuntu 12.04" ]; then
    ubuntu1204
fi
