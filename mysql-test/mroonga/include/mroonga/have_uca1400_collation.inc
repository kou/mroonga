# Copyright (C) 2025  Horimoto Yasuhiro <horimoto@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

--source ../../include/mroonga/check_version.inc
--source ../../include/mroonga/check_mariadb.inc

if (!$mariadb) {
  --skip This test needs utf8mb4_uca1400 family collations
}

if (!$version_11_5_or_later) {
  --skip This test needs utf8mb4_uca1400 family collations
}
