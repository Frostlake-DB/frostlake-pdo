/*
 * Copyright 2026 MLorek
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"

#include "php_pdo_frostlake.h"
#include "php_pdo_frostlake_int.h"

static PHP_MINIT_FUNCTION(pdo_frostlake) {
    zend_declare_class_constant_long(php_pdo_get_dbh_ce(), "FROSTLAKE_STMT_MULTI_STMT_COUNT",
                                     sizeof("FROSTLAKE_STMT_MULTI_STMT_COUNT") - 1,
                                     (zend_long) PDO_FROSTLAKE_ATTR_STMT_MULTI_STMT_COUNT);
    return php_pdo_register_driver(&pdo_frostlake_driver);
}

static PHP_RINIT_FUNCTION(pdo_frostlake) {
#if defined(ZTS) && defined(COMPILE_DL_PDO_FROSTLAKE)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(pdo_frostlake) {
    php_pdo_unregister_driver(&pdo_frostlake_driver);
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(pdo_frostlake) {
    php_info_print_table_start();
    php_info_print_table_row(2, "PDO Driver for Frostlake", "enabled");
    php_info_print_table_row(2, "Version", PHP_PDO_FROSTLAKE_VERSION);
    php_info_print_table_end();
}

static const zend_module_dep pdo_frostlake_deps[] = {
    ZEND_MOD_REQUIRED("pdo")
    ZEND_MOD_END
};

zend_module_entry pdo_frostlake_module_entry = {
    STANDARD_MODULE_HEADER_EX, NULL,
    pdo_frostlake_deps,
    "pdo_frostlake",
    NULL,
    PHP_MINIT(pdo_frostlake),
    PHP_MSHUTDOWN(pdo_frostlake),
    PHP_RINIT(pdo_frostlake),
    NULL,
    PHP_MINFO(pdo_frostlake),
    PHP_PDO_FROSTLAKE_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PDO_FROSTLAKE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(pdo_frostlake)
#endif
