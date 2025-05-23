# -*- mode: sql; sql-product: mysql -*-
#
# Copyright (C) 2012-2024  Sutou Kouhei <kou@clear-code.com>
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

--disable_query_log
let $version_major_minor =
  `SELECT CAST(
    CONCAT(
      SUBSTRING_INDEX(@@global.version, '.', 1),
      '.',
      LPAD(
        SUBSTRING_INDEX(
          SUBSTRING_INDEX(@@global.version, '.', 2),
          '.',
          -1),
        2,
        '0'
      )
    )
    AS DECIMAL(4, 2)
  )`;

let $version_8_0           = `SELECT $version_major_minor = 8.00`;

let $version_8_0_or_later   = `SELECT $version_major_minor >= 8.00`;
let $version_10_11_or_later  = `SELECT $version_major_minor >= 10.11`;
let $version_11_5_or_later  = `SELECT $version_major_minor >= 11.5`;
--enable_query_log
