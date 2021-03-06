# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Andreas-Joachim Peters - CERN
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2011 CERN/Switzerland                                  *
# *                                                                      *
# * This program is free software: you can redistribute it and/or modify *
# * it under the terms of the GNU General Public License as published by *
# * the Free Software Foundation, either version 3 of the License, or    *
# * (at your option) any later version.                                  *
# *                                                                      *
# * This program is distributed in the hope that it will be useful,      *
# * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# * GNU General Public License for more details.                         *
# *                                                                      *
# * You should have received a copy of the GNU General Public License    *
# * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
# ************************************************************************

install(
  FILES eos.keytab
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/
  PERMISSIONS OWNER_READ OWNER_WRITE)

install(
  FILES xrd.cf.fst xrd.cf.mgm xrd.cf.mq xrd.cf.sync xrd.cf.fed
	xrd.cf.prefix xrd.cf.global-mq fuse.conf fuse.conf.eos xrd.cf.auth
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/)

install(
  FILES sysconfig/xrootd.example
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sysconfig/)

install(
  FILES cron.d/eos-logs cron.d/eos-reports cron.d/eos-config
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/cron.d/)

install(
  FILES logrotate.d/eos-logs logrotate.d/eos-fuse-logs
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/logrotate.d/)

# Installing files depending on service manager (systemd or sysVinit)
set(SYSTEMD_DIR /usr/lib/systemd/system/)

if(EXISTS ${SYSTEMD_DIR})
install(
  FILES sysconfig/eos_env.example sysconfig/eosd_env.example
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sysconfig/)

install(DIRECTORY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system/)

install(
  FILES systemd/system/eos.service
	systemd/system/eos@.service
	systemd/system/eos@master.service
	systemd/system/eos@slave.service
	systemd/system/eosd@.service
	systemd/system/eosfstdb@.service
	systemd/system/eosha.service
	systemd/system/eosslave.service
	systemd/system/eossync.service
	systemd/system/eossync@.service
	systemd/system/eossync@config.service
  DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system/)
else()
install(
  FILES sysconfig/eos.example
  DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sysconfig/)

install(
 FILES rc.d/init.d/eos rc.d/init.d/eosha rc.d/init.d/eosslave
	rc.d/init.d/eossync rc.d/init.d/eosd
 DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/rc.d/init.d
 PERMISSIONS OWNER_READ OWNER_EXECUTE
	     GROUP_READ GROUP_EXECUTE
	     WORLD_READ WORLD_EXECUTE)
endif()
