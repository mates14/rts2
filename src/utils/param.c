/*!
 * @file Parameter parsing library.
 * $Id$
 *
 * Functions for parsing parameters. Common for both sides of
 * protocol, server as well as client.
 *
 * @author petr
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <argz.h>

#include "param.h"
#include "hms.h"

int
param_init (struct param_status **params, char *line, char sep)
{
  struct param_status *new_params;

  *params = NULL;

  if (!
      (new_params =
       (struct param_status *) malloc (sizeof (struct param_status))))
    {
      errno = ENOMEM;
      return -1;
    }

  new_params->param_argv = NULL;

  if ((errno =
       argz_create_sep (line, sep, &(new_params->param_argv),
			&(new_params->argz_len))))
    {
      return -1;
    }

  new_params->param_processing = new_params->param_argv;
  *params = new_params;
  return 0;
}

void
param_done (struct param_status *params)
{
  if (params)
    free (params->param_argv);
  free (params);
}

int
param_is_empty (struct param_status *params)
{
  return params->param_argv ? 0 : 1;
}

int
param_get_length (struct param_status *params)
{
  return argz_count (params->param_argv, params->argz_len) - 1;
}

/*!
 * Internel to read next string, and report any errors.
 *
 * @return 0 on succes, -1 and set errno on failure.
 */
int
param_next (struct param_status *params)
{
  if ((params->param_processing =
       argz_next (params->param_argv, params->argz_len,
		  params->param_processing)))
    return 0;
  errno = EINVAL;
  return -1;
}

int
param_next_string (struct param_status *params, char **ret)
{
  if (param_next (params))
    return -1;

  *ret = params->param_processing;
  return 0;
}

int
param_next_integer (struct param_status *params, int *ret)
{
  char *endptr;
  if (param_next (params))
    return -1;
  *ret = strtol (params->param_processing, &endptr, 10);
  if (*endptr)
    {
      errno = EINVAL;
      return -1;
    }
  return 0;
}

int
param_next_float (struct param_status *params, float *ret)
{
  char *endptr;
  if (param_next (params))
    return -1;
  *ret = strtof (params->param_processing, &endptr);
  if (*endptr)
    {
      if (errno != ERANGE)
	errno = EINVAL;
      return -1;
    }
  return 0;
}

int
param_next_hmsdec (struct param_status *params, double *ret)
{
  if (param_next (params))
    return -1;
  *ret = hmstod (params->param_processing);
  if (isnan (*ret))
    {
      errno = EINVAL;
      return -1;
    }
  return 0;
}
