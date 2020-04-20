/* 
 * Copyright (C) 2017  Joshua Choo
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "power-common.h"

// Supported EAS Governors
const char * eas_governors[] = {
    SCHEDUTIL_GOVERNOR,
    NULL
};

void get_int(const char* file_path, int* value, int fallback_value) {
    FILE *file;
    file = fopen(file_path, "r");
    if (file == NULL) {
        *value = fallback_value;
        return;
    }
    fscanf(file, "%d", value);
    fclose(file);
}

void get_hex(const char* file_path, int* value, int fallback_value) {
    FILE *file;
    file = fopen(file_path, "r");
    if (file == NULL) {
        *value = fallback_value;
        return;
    }
    fscanf(file, "0x%x", value);
    fclose(file);
}
