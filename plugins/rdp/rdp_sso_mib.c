/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2026 Remmina contributors
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */

#ifdef WITH_SSO_MIB

#include <sso-mib/sso-mib.h>
#include <freerdp/crypto/crypto.h>
#include <winpr/json.h>
#include <winpr/string.h>

#include "rdp_sso_mib.h"
#include "rdp_plugin.h"

enum remmina_sso_mib_state
{
	SSO_MIB_STATE_INIT = 0,
	SSO_MIB_STATE_FAILED = 1,
	SSO_MIB_STATE_SUCCESS = 2,
};

struct _RemminaSsoMib
{
	MIBClientApp *app;
	enum remmina_sso_mib_state state;
};

static BOOL remmina_sso_mib_get_avd_token(RemminaSsoMib *sso, char **token)
{
	MIBAccount *account = NULL;
	GSList *scopes = NULL;
	MIBPrt *prt;
	BOOL rc = FALSE;

	assert(sso);
	assert(sso->app);
	assert(token);

	*token = NULL;

	account = mib_client_app_get_account_by_upn(sso->app, NULL);
	if (!account)
		goto cleanup;

	scopes = g_slist_append(scopes, g_strdup("https://www.wvd.microsoft.com/.default"));

	prt = mib_client_app_acquire_token_silent(sso->app, account, scopes,
	                                          NULL, NULL, NULL);
	if (prt) {
		const char *access_token = mib_prt_get_access_token(prt);
		if (access_token)
			*token = strdup(access_token);
		g_object_unref(prt);
	}

	rc = (*token != NULL);
cleanup:
	if (account)
		g_object_unref(account);
	g_slist_free_full(scopes, g_free);
	return rc;
}

static BOOL remmina_sso_mib_get_rdsaad_token(RemminaSsoMib *sso, const char *scope,
                                              const char *req_cnf, char **token)
{
	GSList *scopes = NULL;
	WINPR_JSON *json = NULL;
	WINPR_JSON *prop;
	MIBPopParams *params = NULL;
	MIBPrt *prt;
	BOOL rc = FALSE;
	BYTE *req_cnf_dec = NULL;
	size_t req_cnf_dec_len = 0;
	const char *kid;

	assert(sso);
	assert(sso->app);
	assert(scope);
	assert(req_cnf);
	assert(token);

	*token = NULL;

	scopes = g_slist_append(scopes, g_strdup(scope));

	crypto_base64_decode(req_cnf, strlen(req_cnf) + 1, &req_cnf_dec, &req_cnf_dec_len);
	if (!req_cnf_dec)
		goto cleanup;

	json = WINPR_JSON_Parse((const char *)req_cnf_dec);
	if (!json)
		goto cleanup;

	prop = WINPR_JSON_GetObjectItemCaseSensitive(json, "kid");
	if (!prop)
		goto cleanup;

	kid = WINPR_JSON_GetStringValue(prop);
	if (!kid)
		goto cleanup;

	params = mib_pop_params_new(MIB_AUTH_SCHEME_POP, MIB_REQUEST_METHOD_GET, "");
	mib_pop_params_set_kid(params, kid);

	prt = mib_client_app_acquire_token_interactive(sso->app, scopes,
	                                                MIB_PROMPT_NONE, NULL,
	                                                NULL, NULL, params);
	if (prt) {
		const char *access_token = mib_prt_get_access_token(prt);
		if (access_token)
			*token = strdup(access_token);
		rc = (*token != NULL);
		g_object_unref(prt);
	}

cleanup:
	if (params)
		g_object_unref(params);
	WINPR_JSON_Delete(json);
	free(req_cnf_dec);
	g_slist_free_full(scopes, g_free);
	return rc;
}

BOOL remmina_sso_mib_get_access_token(RemminaSsoMib *sso, freerdp *instance,
                                       AccessTokenType tokenType, char **token,
                                       size_t count, const char *scope,
                                       const char *req_cnf)
{
	const char *client_id;
	BOOL rc = FALSE;

	if (!sso)
		return FALSE;

	if (sso->state == SSO_MIB_STATE_FAILED)
		return FALSE;

	if (!sso->app) {
		client_id = freerdp_settings_get_string(instance->context->settings,
		                                        FreeRDP_GatewayAvdClientID);
		sso->app = mib_public_client_app_new(client_id, MIB_AUTHORITY_COMMON,
		                                     NULL, NULL);
	}

	if (!sso->app)
		return FALSE;

	switch (tokenType) {
		case ACCESS_TOKEN_TYPE_AVD:
			rc = remmina_sso_mib_get_avd_token(sso, token);
			if (rc)
				sso->state = SSO_MIB_STATE_SUCCESS;
			else
			{
				REMMINA_PLUGIN_WARNING(
				    "Getting AVD token from identity broker failed, "
				    "falling back to browser-based authentication.");
				sso->state = SSO_MIB_STATE_FAILED;
			}
			break;
		case ACCESS_TOKEN_TYPE_AAD:
		{
			char *scope_decoded = winpr_str_url_decode(scope, strlen(scope));
			if (!scope_decoded) {
				REMMINA_PLUGIN_ERROR("Failed to URL-decode scope");
				break;
			}
			rc = remmina_sso_mib_get_rdsaad_token(sso, scope_decoded, req_cnf,
			                                       token);
			free(scope_decoded);
			if (rc)
				sso->state = SSO_MIB_STATE_SUCCESS;
			else
			{
				REMMINA_PLUGIN_WARNING(
				    "Getting RDS token from identity broker failed, "
				    "falling back to browser-based authentication.");
				sso->state = SSO_MIB_STATE_FAILED;
			}
			break;
		}
		default:
			break;
	}

	return rc;
}

RemminaSsoMib *remmina_sso_mib_new(void)
{
	RemminaSsoMib *sso = calloc(1, sizeof(RemminaSsoMib));
	if (!sso)
		return NULL;

	sso->state = SSO_MIB_STATE_INIT;
	return sso;
}

void remmina_sso_mib_free(RemminaSsoMib *sso)
{
	if (!sso)
		return;

	if (sso->app)
		g_object_unref(sso->app);

	free(sso);
}

#endif /* WITH_SSO_MIB */
