/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2010-2011 Vic Lee
 * Copyright (C) 2014-2015 Antenore Gatta, Fabio Castelli, Giovanni Panozzo
 * Copyright (C) 2016-2023 Antenore Gatta, Giovanni Panozzo
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

#include "rdp_web_auth.h"

#ifdef WITH_SSO_MIB
#include "rdp_sso_mib.h"
#include "rdp_plugin.h"

static BOOL remmina_sso_mib_try_access_token(freerdp *instance, AccessTokenType tokenType,
                                              char **token, size_t count,
                                              const char *scope, const char *req_cnf)
{
	rfContext *rfi = (rfContext *)instance->context;

	if (!rfi->sso_mib)
		rfi->sso_mib = remmina_sso_mib_new();

	if (!rfi->sso_mib)
		return FALSE;

	return remmina_sso_mib_get_access_token(rfi->sso_mib, instance, tokenType, token,
	                                         count, scope, req_cnf);
}
#endif

#ifdef WITH_RDP_AUTH_AAD
#include "rdp_plugin.h"
#include <webkit2/webkit2.h>
#include <freerdp/utils/aad.h>
#include <winpr/wtypes.h>
#include <winpr/wlog.h>

#define AUTH_CANCELLED "__AUTH_CANCELLED__"

#define SET_AUTH_URI(gp, auth_uri) \
	g_object_set_data(G_OBJECT(gp), "auth-uri", auth_uri)

#define GET_AUTH_URI(gp) \
	(gchar*) g_object_get_data(G_OBJECT(gp), "auth-uri")

#define SET_TOKEN_URI(gp, token_uri) \
	g_object_set_data(G_OBJECT(gp), "token-uri", token_uri)

#define GET_TOKEN_URI(gp) \
	(gchar*) g_object_get_data(G_OBJECT(gp), "token-uri")

static char* extract_authorization_code(char* url)
{
	WINPR_ASSERT(url);

	for (char* p = strchr(url, '?'); p++ != NULL; p = strchr(p, '&'))
	{
		if (strncmp(p, "code=", 5) != 0)
			continue;

		char* end = NULL;
		p += 5;

		end = strchr(p, '&');
		if (end)
			*end = '\0';

		return p;
	}

	return NULL;
}

static void delete_event_cb(GtkWidget *dialog, GdkEvent* event, RemminaProtocolWidget *gp) {
	SET_TOKEN_URI(gp, AUTH_CANCELLED);
}

static gboolean decide_policy_cb(WebKitWebView *web_view, WebKitPolicyDecision *decision,
	WebKitPolicyDecisionType type, RemminaProtocolWidget *gp)
{
	WebKitNavigationPolicyDecision *nav_decision;

	switch (type) {
	case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
			nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
			WebKitNavigationAction *nav_action =
				webkit_navigation_policy_decision_get_navigation_action(nav_decision);

		if (webkit_navigation_action_is_redirect(nav_action)) {
			WebKitURIRequest *req = webkit_navigation_action_get_request(nav_action);
			const gchar *uri = webkit_uri_request_get_uri(req);
			SET_TOKEN_URI(gp, uri);
			GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(web_view));
			gtk_widget_destroy(GTK_WIDGET(dialog));
		}
		return FALSE;
	default:
		/* Making no decision results in webkit_policy_decision_use(). */
		return FALSE;
	}
	return TRUE;
}

static BOOL remmina_rdp_webview_show(RemminaProtocolWidget *gp) {
	char* auth_uri = GET_AUTH_URI(gp);

	GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 500, 500);

	WebKitWebView *webView = WEBKIT_WEB_VIEW(webkit_web_view_new());
	gtk_container_add(GTK_CONTAINER(dialog), GTK_WIDGET(webView));

	webkit_web_view_load_uri(webView, auth_uri);

	g_signal_connect(dialog, "delete-event", G_CALLBACK(delete_event_cb), gp);
	g_signal_connect(webView, "decide-policy", G_CALLBACK(decide_policy_cb), gp);

	gtk_widget_show_all(dialog);

	return G_SOURCE_REMOVE;
}

static BOOL remmina_rdp_get_rdsaad_access_token(freerdp* instance, const char* scope,
	const char* req_cnf, char** token)
{
	assert(instance);
	assert(instance->context);

	rfContext *rfi;
	RemminaProtocolWidget *gp;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;

	char* token_request = NULL;
	size_t token_request_len = 0;
	char* redirect_uri = NULL;
	size_t redirect_uri_len = 0;
	char* auth_uri = NULL;
	size_t auth_uri_len = 0;

	assert(scope);
	assert(req_cnf);
	assert(token);

	BOOL rc = FALSE;
	*token = NULL;

	const char* client_id =
	    freerdp_settings_get_string(instance->context->settings, FreeRDP_GatewayAvdClientID);
	if (!client_id)
		goto cleanup;

	winpr_asprintf(&redirect_uri, &redirect_uri_len,
	               "ms-appx-web%%3a%%2f%%2fMicrosoft.AAD.BrokerPlugin%%2f%s", client_id);
	if (!redirect_uri)
		goto cleanup;

	const char* ep = freerdp_utils_aad_get_wellknown_string(instance->context,
	                                                        AAD_WELLKNOWN_authorization_endpoint);

	winpr_asprintf(&auth_uri, &auth_uri_len, "%s?client_id=%s&response_type="
	       "code&scope=%s&redirect_uri=%s",
	       ep, client_id, scope, redirect_uri);
	if (!auth_uri) {
		goto cleanup;
	}

	SET_AUTH_URI(gp, auth_uri);
	IDLE_ADD((GSourceFunc)remmina_rdp_webview_show, gp);

	char* token_uri;
	while(true) {
		usleep(500 * 1000);
		token_uri = GET_TOKEN_URI(gp);
		if (token_uri == NULL) {
			continue;
		}

		if (g_str_equal(token_uri, AUTH_CANCELLED)) {
			rc = FALSE;
			goto cleanup;
		}

		char* code = extract_authorization_code(token_uri);
		if (!code) {
			goto cleanup;
		}

		if (winpr_asprintf(&token_request, &token_request_len,
						"grant_type=authorization_code&code=%s&client_id=%s&scope=%s&redirect_uri=%"
						"s&req_cnf=%s",
						code, client_id, scope, redirect_uri, req_cnf) <= 0)
		{
			goto cleanup;
		}

		rc = client_common_get_access_token(instance, token_request, token);
		break;
	}

cleanup:
	free(auth_uri);
	free(redirect_uri);
	free(token_request);
	SET_AUTH_URI(gp, NULL);
	SET_TOKEN_URI(gp, NULL);
	return rc && (*token != NULL);
}

static BOOL remmina_rdp_get_avd_access_token(freerdp* instance, char** token) {
	assert(instance);
	assert(instance->context);

	rfContext *rfi;
	RemminaProtocolWidget *gp;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;

	char* token_request = NULL;
	size_t token_request_len = 0;
	char* redirect_uri = NULL;
	size_t redirect_uri_len = 0;
	char* auth_uri = NULL;
	size_t auth_uri_len = 0;
	const char* scope = "https%3A%2F%2Fwww.wvd.microsoft.com%2F.default";

	assert(token);

	BOOL rc = FALSE;
	*token = NULL;

	const char* client_id =
	    freerdp_settings_get_string(instance->context->settings, FreeRDP_GatewayAvdClientID);
	const char* base = freerdp_settings_get_string(instance->context->settings,
	                                               FreeRDP_GatewayAzureActiveDirectory);
	const BOOL useTenant =
	    freerdp_settings_get_bool(instance->context->settings, FreeRDP_GatewayAvdUseTenantid);
	const char* tenantid = "common";
	if (useTenant) {
		tenantid =
		    freerdp_settings_get_string(instance->context->settings, FreeRDP_GatewayAvdAadtenantid);
	}
	if (!base || !tenantid || !client_id) {
		goto cleanup;
	}

	winpr_asprintf(&redirect_uri, &redirect_uri_len,
	               "https%%3A%%2F%%2F%s%%2F%s%%2Foauth2%%2Fnativeclient", base, tenantid);
	if (!redirect_uri) {
		goto cleanup;
	}

	const char* ep = freerdp_utils_aad_get_wellknown_string(instance->context,
	                                                        AAD_WELLKNOWN_authorization_endpoint);

	winpr_asprintf(&auth_uri, &auth_uri_len, "%s?client_id=%s&response_type="
	       "code&scope=%s&redirect_uri=%s",
	       ep, client_id, scope, redirect_uri);
	if (!auth_uri) {
		goto cleanup;
	}

	SET_AUTH_URI(gp, auth_uri);
	IDLE_ADD((GSourceFunc)remmina_rdp_webview_show, gp);

	char* token_uri;
	while(true) {
		usleep(500 * 1000);
		token_uri = GET_TOKEN_URI(gp);
		if (token_uri == NULL)
		{
			continue;
		}

		if (g_str_equal(token_uri, AUTH_CANCELLED))
		{
			rc = FALSE;
			goto cleanup;
		}

		char* code = extract_authorization_code(token_uri);
		if (!code) {
			goto cleanup;
		}

		if (winpr_asprintf(
				&token_request, &token_request_len,
				"grant_type=authorization_code&code=%s&client_id=%s&scope=%s&redirect_uri=%s", code,
				client_id, scope, redirect_uri) <= 0)
		{
			goto cleanup;
		}

		rc = client_common_get_access_token(instance, token_request, token);
		break;
	}

cleanup:
	free(auth_uri);
	free(redirect_uri);
	free(token_request);
	SET_AUTH_URI(gp, NULL);
	SET_TOKEN_URI(gp, NULL);
	return rc && (*token != NULL);
}

BOOL remmina_rdp_get_access_token(freerdp* instance, AccessTokenType tokenType, char** token,
									size_t count, ...)
{
	const char* scope = NULL;
	const char* req_cnf = NULL;

	va_list ap = { 0 };
	va_start(ap, count);
	if (tokenType == ACCESS_TOKEN_TYPE_AAD)
	{
		if (count < 2)
		{
			REMMINA_PLUGIN_ERROR(
			         "ACCESS_TOKEN_TYPE_AAD expected 2 additional arguments, but got %zu, aborting",
			         count);
			va_end(ap);
			return FALSE;
		}
		else if (count > 2) {
			REMMINA_PLUGIN_WARNING(
			          "ACCESS_TOKEN_TYPE_AAD expected 2 additional arguments, but got %zu, ignoring",
			          count);
		}
		scope = va_arg(ap, const char*);
		req_cnf = va_arg(ap, const char*);
	}
	else if (tokenType == ACCESS_TOKEN_TYPE_AVD)
	{
		if (count != 0) {
			REMMINA_PLUGIN_WARNING(
			          "ACCESS_TOKEN_TYPE_AVD expected 0 additional arguments, but got %zu, ignoring",
			          count);
		}
	}
	va_end(ap);

#ifdef WITH_SSO_MIB
	if (remmina_sso_mib_try_access_token(instance, tokenType, token, count, scope, req_cnf))
		return TRUE;
#endif

	switch (tokenType)
	{
		case ACCESS_TOKEN_TYPE_AAD:
			return remmina_rdp_get_rdsaad_access_token(instance, scope, req_cnf, token);
		case ACCESS_TOKEN_TYPE_AVD:
			return remmina_rdp_get_avd_access_token(instance, token);
		default:
			REMMINA_PLUGIN_ERROR("Unexpected value for AccessTokenType [%" PRIuz "], aborting", tokenType);
			return FALSE;
	}
}
#else
#if FREERDP_VERSION_MAJOR >= 3
BOOL remmina_rdp_get_access_token(freerdp* instance, AccessTokenType tokenType, char** token,
                                    size_t count, ...)
{
#ifdef WITH_SSO_MIB
	const char* scope = NULL;
	const char* req_cnf = NULL;

	va_list ap = { 0 };
	va_start(ap, count);
	if (tokenType == ACCESS_TOKEN_TYPE_AAD && count >= 2)
	{
		scope = va_arg(ap, const char*);
		req_cnf = va_arg(ap, const char*);
	}
	va_end(ap);

	if (remmina_sso_mib_try_access_token(instance, tokenType, token, count, scope, req_cnf))
		return TRUE;
#endif
	return client_cli_get_access_token(instance, tokenType, token, count);
}
#endif
#endif
