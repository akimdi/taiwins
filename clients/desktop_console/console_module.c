/*
 * console_module.c - taiwins client console module functions
 *
 * Copyright (c) 2019-2020 Xichen Zhou
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

#include <ctypes/vector.h>
#include "console.h"

/******************************************************************************/
void
search_entry_assign(void *dst, const void *src)
{
	console_search_entry_t *d = dst;
	const console_search_entry_t *s = src;

	search_entry_free(dst);
	strncpy(d->sstr, s->sstr, 32);
	if (s->pstr)
		d->pstr = strdup(s->pstr);
	d->img = s->img;
}

void
search_entry_free(void *m)
{
	console_search_entry_t *entry = m;
	if (entry->pstr) {
		free(entry->pstr);
		entry->pstr = NULL;
	}
	entry->sstr[0] = '\0';
}

bool
search_entry_equal(console_search_entry_t *l, console_search_entry_t *r)
{
	int lcmp = 0;
	int scmp = strncmp(l->sstr, r->sstr, 32);

	if (!l->pstr && !r->pstr)
		lcmp = 0;
	else if (l->pstr && r->pstr)
		lcmp = strcmp(l->pstr, r->pstr);
	else
		lcmp = 1;
	return ((lcmp == 0) && (scmp == 0)) ? true : false;
}

/******************************************************************************/

/**
 * @brief general scenario
 *
 * console main thread is issuing search commands to module threads. When it
 * does so, it should first clean out the search results if any (there could be
 * cases where console thread is issuing the command while module is doing the
 * search as well. Such that By the time it finished, new commands is coming in,
 * so at the module side, checking )
 */

struct module_search_cache {
	char *last_command;
	vector_t last_results;
};


static inline void
cache_takes(struct module_search_cache *cache,
	    vector_t *v, const char *command)
{
	if (cache->last_command)
		free(cache->last_command);
	cache->last_command = strdup(command);
	if (cache->last_results.elems) {
		vector_destroy(&cache->last_results);
	}
	vector_copy_complex(&cache->last_results, v,
			    search_entry_assign);
}

static inline void
cache_init(struct module_search_cache *cache)
{
	cache->last_command = NULL;
	cache->last_results = (vector_t){0};
}

static inline void
cache_free(struct module_search_cache *cache)
{
	if (cache->last_command)
		free(cache->last_command);
	if (cache->last_results.elems)
		vector_destroy(&cache->last_results);
	cache_init(cache);
}

static void
cache_filter(struct module_search_cache *cache,
	     char *command, vector_t *v,
	     bool (*filter_test)(const char *cmd, const char *candidate))
{
	console_search_entry_t *entry = NULL;
	int cmp = strcmp(cache->last_command, command);

	if (cmp == 0)
		return;
	else if (cmp < 0) {
		vector_init_zero(v, sizeof(console_search_entry_t),
				 search_entry_free);
		vector_for_each(entry, &cache->last_results) {
			const char *str = search_entry_get_string(entry);
			if (filter_test(command, str)) {
				search_entry_move(vector_newelem(v), entry);
			}
		}
	} else //cachable ensures that cmp will not be positive
		assert(0);

	cache_free(cache);
	cache_takes(cache, v, command);
}

static inline bool
cachable(const struct module_search_cache *cache,
	 char *command)
{
	return command != NULL && cache->last_command != NULL &&
		strstr(command, cache->last_command) == command &&
		strcmp(cache->last_command, command) <= 0;
}

/**
 * @brief running thread for the module,
 *
 * module thread is a consumer for commands and producer for results. If the
 * results is not taken, it needs to clean up before do it again.
 *
 * Console thread does the reverse. It is the consumer of results and producer
 * of commands, if the commands it produced is not taken, it needs to reset it
 * manually.
 *
 * We have a general cache method to reduce the uncessary module searching.
 */
void *
thread_run_module(void *arg)
{
	struct console_module *module = arg;
	char *exec_command = NULL, *search_command = NULL;
	char *exec_res = NULL;
	vector_t search_results = {0};
	int exec_ret = 0, search_ret = 0;
	struct module_search_cache cache;

	cache_init(&cache);

	while (!module->quit) {
		//exec, enter critial
		pthread_mutex_lock(&module->command_mutex);
		if (module->exec_command) {
			exec_command = module->exec_command;
			module->exec_command = NULL;
		}
		pthread_mutex_unlock(&module->command_mutex);
		if (exec_command) {
			exec_ret = module->exec(module, exec_command,
						&exec_res);
			free(exec_command);
			exec_command = NULL;
		}

		//again, if results are not taken, we need to free it.
		pthread_mutex_lock(&module->results_mutex);
		if (module->exec_res) {
			free(module->exec_res);
			module->exec_res = NULL;
		}
		if (exec_res)
			module->exec_res = exec_res;
		module->exec_ret = exec_ret;
		exec_res = NULL;
		pthread_mutex_unlock(&module->results_mutex);

		//search
		pthread_mutex_lock(&module->command_mutex);
		if (module->search_command) {
			search_command = module->search_command;
			module->search_command = NULL;
		}
		pthread_mutex_unlock(&module->command_mutex);
		//deal with cache first
		if (module->support_cache && cachable(&cache, search_command))
			cache_filter(&cache, search_command, &search_results,
				module->filter_test);
		else if (search_command) {
			search_ret = module->search(module, search_command,
						    &search_results);
			//fprintf(stderr, "search for %s has %d results\n", search_command,
			//	search_results.len);
			cache_takes(&cache, &search_results, search_command);
			free(search_command);
			search_command = NULL;
		}

		pthread_mutex_lock(&module->results_mutex);
		if (module->search_results.elems)
			vector_destroy(&module->search_results);
		module->search_results = search_results;
		module->search_ret = search_ret;
		search_results = (vector_t){0};
		search_ret = 0;
		pthread_mutex_unlock(&module->results_mutex);

		/* int value; */
		/* sem_getvalue(&module->semaphore, &value); */
		/* fprintf(stderr, "sem value now : %d\n", value); */
		sem_wait(&module->semaphore);
	}
	cache_free(&cache);
	return NULL;
}

static bool
console_module_filter_test(const char *cmd, const char *entry)
{
	return (strstr(entry, cmd) == entry);
}

/******************************************************************************/

void
console_module_init(struct console_module *module,
                    struct desktop_console *console)
{
	module->console = console;
	pthread_mutex_init(&module->command_mutex, NULL);
	pthread_mutex_init(&module->results_mutex, NULL);
	sem_init(&module->semaphore, 0, 0);
	pthread_create(&module->thread, NULL, thread_run_module,
		       (void *)module);
	if (module->init_hook)
		module->init_hook(module);
	if (!module->filter_test)
		module->filter_test = console_module_filter_test;

	//cleanup the module data
	module->exec_command = NULL;
	module->search_command = NULL;
	module->search_ret = 0;
	module->exec_ret = 0;
	vector_init_zero(&module->search_results,
	                 sizeof(console_search_entry_t), search_entry_free);
	module->exec_res = NULL;
	module->quit = false;
}

void
console_module_release(struct console_module *module)
{
	int lock_state = -1;

	module->quit = true;
	//wake the threads
	while (lock_state <= 0) {
		sem_getvalue(&module->semaphore, &lock_state);
		sem_post(&module->semaphore);
	}
	pthread_join(module->thread, NULL);

	pthread_mutex_lock(&module->command_mutex);
	if (module->search_command) {
		free(module->search_command);
		module->search_command = NULL;
	}
	if (module->exec_command) {
		free(module->exec_command);
		module->exec_command = NULL;
	}
	pthread_mutex_unlock(&module->command_mutex);

	pthread_mutex_destroy(&module->command_mutex);
	pthread_mutex_destroy(&module->results_mutex);
	sem_destroy(&module->semaphore);

	if (module->search_results.elems)
		vector_destroy(&module->search_results);
	if (module->destroy_hook)
		module->destroy_hook(module);
}

void
console_module_command(struct console_module *module,
		       const char *search, const char *exec)
{
	if (search && strlen(search)) {
		pthread_mutex_lock(&module->command_mutex);
		if (module->search_command)
			free(module->search_command);
		module->search_command = strdup(search);
		pthread_mutex_unlock(&module->command_mutex);
		sem_post(&module->semaphore);
	}
	if (exec && strlen(exec)) {
		pthread_mutex_lock(&module->command_mutex);
		if (module->exec_command)
			free(module->exec_command);
		module->exec_command = strdup(exec);
		pthread_mutex_unlock(&module->command_mutex);
		sem_post(&module->semaphore);
	}
	/* int value; */
	/* sem_getvalue(&module->semaphore, &value); */
	/* fprintf(stderr, "sem value now : %d\n", value); */
}

int
desktop_console_take_search_result(struct console_module *module,
				  vector_t *ret)
{
	int retcode = 0;

	if (pthread_mutex_trylock(&module->results_mutex))
		return 0;
	if (module->search_results.len == -1) //taken
		;
	else {
		vector_destroy(ret);
		*ret = module->search_results;
		module->search_results = (vector_t){0};
		module->search_results.len = -1; //taken
		retcode = module->search_ret;
		module->search_ret = 0;
	}
	pthread_mutex_unlock(&module->results_mutex);
	return retcode;
}

int
desktop_console_take_exec_result(struct console_module *module,
				char **result)
{
	int retcode = 0;
	if (pthread_mutex_trylock(&module->results_mutex)) {
		*result = NULL;
		return 0;
	}
	//pthread_mutex_lock(&module->results_mutex);
	if (module->exec_res)
		*result = module->exec_res;
	module->exec_res = NULL;
	retcode = module->exec_ret;
	module->exec_ret = 0;
	pthread_mutex_unlock(&module->results_mutex);

	return retcode;
}
