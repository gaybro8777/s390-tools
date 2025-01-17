/*
 * zdev - Modify and display the persistent configuration of devices
 *
 * Copyright IBM Corp. 2016, 2017
 *
 * s390-tools is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/util_path.h"
#include "lib/util_udev.h"

#include "attrib.h"
#include "ccw.h"
#include "device.h"
#include "misc.h"
#include "path.h"
#include "setting.h"
#include "udev.h"

int udev_need_settle = 0;
int udev_no_settle;

/* Check if a udev file does not contain any statements. */
bool udev_file_is_empty(struct util_udev_file *file)
{
	struct util_udev_line_node *l;

	if (!file)
		return true;
	util_list_iterate(&file->lines, l) {
		if (l->line[0])
			return false;
	}

	return true;
}

static bool get_ids_cb(const char *filename, void *data)
{
	char *prefix = data;

	if (strncmp(filename, prefix, strlen(prefix)) != 0)
		return false;
	if (!ends_with(filename, UDEV_SUFFIX))
		return false;

	return true;
}

/* Add the IDs for all devices of the specified subtype name for which a
 * udev rule exists to strlist LIST. */
void udev_get_device_ids(const char *type, struct util_list *list,
			 bool autoconf)
{
	char *path, *prefix;
	struct util_list *files;
	struct strlist_node *s;
	size_t plen, len;

	prefix = misc_asprintf("%s-%s-", UDEV_PREFIX, type);
	plen = strlen(prefix);
	path = path_get_udev_rules(autoconf);
	files = strlist_new();
	if (!misc_read_dir(path, files, get_ids_cb, prefix))
		goto out;

	util_list_iterate(files, s) {
		/* 41-dasd-eckd-0.0.1234.rules */
		len = strlen(s->str);
		s->str[len - sizeof(UDEV_SUFFIX) + 1] = 0;
		strlist_add(list, &s->str[plen]);
	}

out:
	strlist_free(files);
	free(path);
	free(prefix);
}

/* Remove UDEV rule for device. */
exit_code_t udev_remove_rule(const char *type, const char *id, bool autoconf)
{
	char *path;
	exit_code_t rc = EXIT_OK;

	path = path_get_udev_rule(type, id, autoconf);
	if (util_path_is_reg_file(path))
		rc = remove_file(path);
	free(path);

	return rc;
}

/* Wait for all current udev events to finish. */
void udev_settle(void)
{
	if (udev_no_settle)
		return;
	misc_system(err_ignore, "%s settle", PATH_UDEVADM);
}

/* Extract internal attribute settings from @entry and add to @list.
 * Associate corresponding attribute if found in @attribs. */
void udev_add_internal_from_entry(struct setting_list *list,
				  struct util_udev_entry_node *entry,
				  struct attrib **attribs)
{
	char *copy, *name, *end, *u;
	struct attrib *a;

	/* ENV{zdev_var}="1" */
	copy = misc_strdup(entry->key);

	/* Find attribute name start. */
	name = strchr(copy, '{');
	end = strrchr(copy, '}');
	if (!name || !end)
		goto out;
	*end = 0;
	name++;

	/* zdev_ => zdev: */
	u = strchr(name, '_');
	if (u)
		*u = ':';

	a = attrib_find(attribs, name);
	setting_list_apply_actual(list, a, name, entry->value);

out:
	free(copy);
}
