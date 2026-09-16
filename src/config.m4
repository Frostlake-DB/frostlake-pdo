dnl Copyright 2026 MLorek
dnl
dnl Licensed under the Apache License, Version 2.0 (the "License");
dnl you may not use this file except in compliance with the License.
dnl You may obtain a copy of the License at
dnl
dnl     http://www.apache.org/licenses/LICENSE-2.0
dnl
dnl Unless required by applicable law or agreed to in writing, software
dnl distributed under the License is distributed on an "AS IS" BASIS,
dnl WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
dnl See the License for the specific language governing permissions and
dnl limitations under the License.

dnl The phpize build lives in src/ so that its generated Makefile never replaces the repository's.

PHP_ARG_ENABLE([pdo_frostlake],
  [whether to enable the PDO driver for Frostlake],
  [AS_HELP_STRING([--enable-pdo-frostlake], [Enable the PDO driver for Frostlake])],
  [yes])

if test "$PHP_PDO_FROSTLAKE" != "no"; then
  PHP_CHECK_PDO_INCLUDES
  PHP_NEW_EXTENSION([pdo_frostlake],
    [pdo_frostlake.c frostlake_driver.c frostlake_statement.c frostlake_sql.c frostlake_wire.c json.c http.c],
    [$ext_shared],,
    [-D_GNU_SOURCE])
  PHP_ADD_EXTENSION_DEP([pdo_frostlake], [pdo])
fi
