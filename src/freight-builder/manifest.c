
/*********************************************************
 *Copyright (C) 2015 Neil Horman
 *This program is free software; you can redistribute it and\or modify
 *it under the terms of the GNU General Public License as published 
 *by the Free Software Foundation; either version 2 of the License,
 *or  any later version.
 *
 *This program is distributed in the hope that it will be useful,
 *but WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *GNU General Public License for more details.
 *
 *File: manifest.c
 *
 *Author:Neil Horman
 *
 *Date: 4/9/2015
 *
 *Description
 * *********************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libconfig.h>
#include "manifest.h"

void release_manifest(struct manifest *manifest)
{
	struct repository *repos = manifest->repos;
	struct rpm *rpms = manifest->rpms;
	void *killer;

	while (rpms) {
		killer = rpms;
		rpms = rpms->next;
		free(killer);
	}

	while (repos) {
		killer = repos;
		repos = repos->next;
		free(killer);
	}

	if (manifest->options)
		free(manifest->options);

	free(manifest->package.license);
	free(manifest->package.summary);
	free(manifest->package.release);
	free(manifest->package.version);
	free(manifest->package.name);

	memset(manifest, 0, sizeof(struct manifest));
}

static int parse_repositories(struct config_t *config, struct manifest *manifest)
{
	config_setting_t *repos = config_lookup(config, "repositories");
	config_setting_t *repo, *tmp;
	struct repository *repop;
	int i = 0;
	size_t alloc_size;
	const char *name, *url;

	if (!repos)
		return 0;

	/*
 	 * Load up all the individual repository urls
 	 */
	while ((repo = config_setting_get_elem(repos, i)) != NULL) {

		tmp = config_setting_get_member(repo, "name");
		if (!tmp)
			return -EINVAL;
		name = config_setting_get_string(tmp);
		if (!name)
			return -EINVAL;
		tmp = config_setting_get_member(repo, "url");
		if (!tmp)
			return -EINVAL;
		url = config_setting_get_string(tmp);
		if (!url)
			return -EINVAL;

		/*
 		 * Add two for the null terminators
 		 */
		alloc_size = strlen(name) + strlen(url) + 2;

		repop = calloc(1, sizeof(struct repository) + alloc_size);
		if (!repop)
			return -ENOMEM;

		/*
 		 * Allocate memory for strings at the end of the repository
 		 * structure so that we can free it as a unit
 		 */
		repop->name = (char *)((void *)repop + sizeof(struct repository));
		/*
 		 * Add one for the null terminator
 		 */
		repop->url = (char *)((void *)repop + sizeof(struct repository) + strlen(name))+1;
		strcpy(repop->name, name);
		strcpy(repop->url, url); 

		repop->next = manifest->repos;
		manifest->repos = repop;	
		
		i++;
	}

	return 0;
}

static int parse_rpms(struct config_t *config, struct manifest *manifest)
{
	config_setting_t *rpms = config_lookup(config, "manifest");
	config_setting_t *rpm_config;
	struct rpm *rpmp;
	int i = 0;
	size_t alloc_size;
	const char *name;

	if (!rpms)
		return 0;

	/*
 	 * Load up all the individual repository urls
 	 */
	while ((rpm_config = config_setting_get_elem(rpms, i)) != NULL) {

		name = config_setting_get_string(rpm_config);
		if (!name)
			return -EINVAL;

		alloc_size = strlen(name);

		rpmp = calloc(1, sizeof(struct rpm) + alloc_size);
		if (!rpmp)
			return -ENOMEM;

		/*
 		 * Allocate memory for strings at the end of the repository
 		 * structure so that we can free it as a unit
 		 */
		rpmp->name = (char *)(rpmp + 1);
		strcpy(rpmp->name, name);

		rpmp->next = manifest->rpms;
		manifest->rpms = rpmp;	
		
		i++;
	}

	return 0;
}

static int parse_packaging(struct config_t *config, struct manifest *manifest)
{
	config_setting_t *pkg = config_lookup(config, "packaging");
	config_setting_t *tmp;
	int rc = -EINVAL;

	if (!pkg) {
		fprintf(stderr, "You must supply a packaging directive\n");
		goto out;
	}

	tmp = config_setting_get_member(pkg, "name");
	if (!tmp) {
		fprintf(stderr, "You must specify a package name\n");
		goto out;
	}
	manifest->package.name = strdup(config_setting_get_string(tmp));

	tmp = config_setting_get_member(pkg, "version");
	if (!tmp) {
		fprintf(stderr, "You must specify a package version\n");
		goto out_name;
	}
	manifest->package.version = strdup(config_setting_get_string(tmp));

	tmp = config_setting_get_member(pkg, "release");
	if (!tmp) {
		fprintf(stderr, "You must specify a package release\n");
		goto out_version;
	}
	manifest->package.release = strdup(config_setting_get_string(tmp));

	tmp = config_setting_get_member(pkg, "summary");
	if (!tmp) {
		fprintf(stderr, "You must specify a package summary\n");
		goto out_release;
	}
	manifest->package.summary = strdup(config_setting_get_string(tmp));

	tmp = config_setting_get_member(pkg, "license");
	if (!tmp) {
		fprintf(stderr, "You must specify a package license\n");
		goto out_summary;
	}
	manifest->package.license = strdup(config_setting_get_string(tmp));

	tmp = config_setting_get_member(pkg, "author");
	if (!tmp) {
		fprintf(stderr, "You must specify a package author\n");
		goto out_license;
	}
	manifest->package.author = strdup(config_setting_get_string(tmp));

	rc = 0;
	goto out;

out_license:
	free(manifest->package.license);
out_summary:
	free(manifest->package.summary);
out_release:
	free(manifest->package.release);
out_version:
	free(manifest->package.version);
out_name:
	free(manifest->package.name);
out:
	return rc;
}

static int __read_manifest(const char *config_path, struct manifest *manifest,
			   int base)
{
	int rc = 0;
	const char *next_path;
	config_t config;
	struct stat buf;

	/*
 	 * initalize the config structure
 	 */
	config_init(&config);

	/*
 	 * Check for file existance
 	 */
	if (stat(config_path, &buf)) {
		fprintf(stderr, "Error, manifest file %s does not exist\n",
			config_path);
		rc = -ENOENT;
		goto out;
	}

	if (config_read_file(&config, config_path) == CONFIG_FALSE) {
		fprintf(stderr, "Error in %s:%d : %s\n", 
			config_error_file(&config), config_error_line(&config),
			config_error_text(&config));
		rc = -EINVAL;
		goto out;
	}

	/*
 	 * Good config file, look for an inherit directive
 	 */
	if (config_lookup_string(&config, "inherit", &next_path) == CONFIG_TRUE) {
		/*
 		 * Recursively parse the parent manifest
 		 */
		rc = __read_manifest(next_path, manifest, 0);
		if (rc) 
			goto out;
	}


	/*
 	 * Now add in our manifest directives
 	 */
	rc = parse_repositories(&config, manifest);
	if (rc)
		goto out;

	rc = parse_rpms(&config, manifest);
	if (rc)
		goto out;

	if (!base) {
		if (config_lookup(&config, "packaging") != NULL) {
			fprintf(stderr, "Can't include packaging directive in "
				"inherited manifest %s\n", config_path);
			rc = -EINVAL;
			goto out;
		}
	} else {
		rc = parse_packaging(&config, manifest);
		if (rc)
			goto out;
	}
out:
	config_destroy(&config);
	return rc;
}

int read_manifest(char *config_path, struct manifest *manifestp)
{
	int rc;

	memset(manifestp, 0, sizeof(struct manifest));

	rc = __read_manifest(config_path, manifestp, 1);

	if (rc) {
		release_manifest(manifestp);
		goto out;
	}

out:
	return rc;
}