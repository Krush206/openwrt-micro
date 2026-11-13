/*
 * index.c — OpenWrt minimalist administrator CGI (C + libuci 0.7)
 *
 * Built with the mipsel-uClibc cross toolchain and installed under
 * www/cgi-bin/index so busybox httpd (document root /www) executes it.
 *
 * Reads/writes /etc/config/system via libuci (libuci.so.0.7) and renders a
 * Bootstrap 3 control panel. No jQuery — only the Bootstrap 3 CSS that was
 * downloaded into www/bootstrap/.
 *
 * libuci API (vendored at package/uci/src/):
 *   uci_alloc_context / uci_free_context
 *   uci_set_confdir(ctx, "/etc/config")
 *   uci_load(ctx, "system", &pkg)
 *   uci_lookup_ptr(ctx, &ptr, NULL, true)   // extended tuple lookup
 *   uci_set(ctx, &ptr)                       // ptr->value must be set
 *   uci_commit(ctx, &ptr.p, true)            // persist
 *   uci_get_errorstr(ctx, &dest, prefix)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "uci.h"

#define UCI_CONFDIR   "/etc/config"
#define UCI_SYSTEM    "system"
#define UCI_SECTION   "@system[0]"

static char *url_decode(const char *src, char *dst, size_t dstsz)
{
	size_t di = 0;
	size_t i;
	for (i = 0; src[i] && di + 1 < dstsz; i++) {
		char c = src[i];
		if (c == '+') {
			c = ' ';
		} else if (c == '%' && isxdigit((unsigned char)src[i + 1]) &&
			 isxdigit((unsigned char)src[i + 2])) {
			char h[3] = { src[i + 1], src[i + 2], 0 };
			c = (char)strtol(h, NULL, 16);
			i += 2;
		}
		dst[di++] = c;
	}
	dst[di] = '\0';
	return dst;
}

/* Write the value of `key` from an application/x-www-form-urlencoded string
 * `form` into `out` (at most outsz bytes). Returns 1 if found, 0 otherwise. */
static int form_field(const char *form, const char *key, char *out, size_t outsz)
{
	const char *p = form;
	out[0] = '\0';
	if (!form) return 0;
	while (*p) {
		const char *eq = strchr(p, '=');
		const char *amp;
		char kbuf[128], vbuf[512];
		char *v;
		if (!eq) break;
		size_t klen = (size_t)(eq - p);
		if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
		memcpy(kbuf, p, klen);
		kbuf[klen] = '\0';

		vbuf[0] = '\0';
		v = (char *)(eq + 1);
		amp = strchr(v, '&');
		if (amp) {
			size_t vlen = (size_t)(amp - v);
			if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
			memcpy(vbuf, v, vlen);
			vbuf[vlen] = '\0';
		} else {
			strncpy(vbuf, v, sizeof(vbuf) - 1);
			vbuf[sizeof(vbuf) - 1] = '\0';
		}

		url_decode(kbuf, kbuf, sizeof(kbuf));
		url_decode(vbuf, vbuf, sizeof(vbuf));

		if (strcmp(kbuf, key) == 0) {
			strncpy(out, vbuf, outsz - 1);
			out[outsz - 1] = '\0';
			return 1;
		}

		if (amp) p = amp + 1;
		else break;
	}
	return 0;
}

static void html_escape(const char *s, FILE *out)
{
	if (!s) s = "";
	for (; *s; s++) {
		switch (*s) {
		case '<': fputs("&lt;", out); break;
		case '>': fputs("&gt;", out); break;
		case '&': fputs("&amp;", out); break;
		case '"': fputs("&quot;", out); break;
		default:  fputc(*s, out); break;
		}
	}
}

/* Look up a UCI tuple and return its string value. Returns pointer to buf on
 * success, NULL if not found or not a string option. */
static const char *uci_get_value(struct uci_context *ctx,
				 const char *package, const char *section,
				 const char *option, char *buf, size_t bufsz)
{
	struct uci_ptr ptr;
	buf[0] = '\0';
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = package;
	ptr.section = section;
	ptr.option = option;

	if (uci_lookup_ptr(ctx, &ptr, NULL, true) != UCI_OK)
		return NULL;
	if (ptr.o && ptr.o->type == UCI_TYPE_STRING && ptr.o->v.string) {
		strncpy(buf, ptr.o->v.string, bufsz - 1);
		buf[bufsz - 1] = '\0';
		return buf;
	}
	return NULL;
}

/* Set a UCI option value and commit the package. Returns 0 on success. */
static int uci_set_value(struct uci_context *ctx,
			 const char *package, const char *section,
			 const char *option, const char *value)
{
	struct uci_ptr ptr;
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = package;
	ptr.section = section;
	ptr.option = option;
	ptr.value = value;

	if (uci_lookup_ptr(ctx, &ptr, NULL, true) != UCI_OK)
		return -1;
	if (uci_set(ctx, &ptr) != UCI_OK)
		return -1;
	if (uci_commit(ctx, &ptr.p, true) != UCI_OK)
		return -1;
	return 0;
}

int main(void)
{
	const char *method = getenv("REQUEST_METHOD") ? getenv("REQUEST_METHOD") : "GET";
	const char *query  = getenv("QUERY_STRING")  ? getenv("QUERY_STRING")  : "";
	char *postbody = NULL;
	char *form = (char *)query;
	char action_msg[256] = "";
	char action_type[16] = "info";

	/* --- read request body for POST --- */
	if (strcmp(method, "POST") == 0) {
		char *clenv = getenv("CONTENT_LENGTH");
		long cl = clenv ? strtol(clenv, NULL, 10) : 0;
		if (cl > 0 && cl < 1024 * 1024) {
			postbody = (char *)malloc((size_t)cl + 1);
			if (postbody) {
				size_t n = fread(postbody, 1, (size_t)cl, stdin);
				postbody[n] = '\0';
				form = postbody;
			}
		}
	}

	/* --- libuci context --- */
	struct uci_context *ctx = uci_alloc_context();
	if (!ctx) {
		fprintf(stderr, "uci_alloc_context failed\n");
		return 1;
	}
	if (uci_set_confdir(ctx, UCI_CONFDIR) != UCI_OK) {
		/* non-fatal; fall back to default */
	}
	struct uci_package *pkg = NULL;
	uci_load(ctx, UCI_SYSTEM, &pkg);

	/* --- handle form submission --- */
	char new_hostname[128] = "";
	char new_timezone[64] = "";
	if (strcmp(method, "POST") == 0) {
		form_field(form, "hostname", new_hostname, sizeof(new_hostname));
		form_field(form, "timezone", new_timezone, sizeof(new_timezone));

		int rc = 0;
		if (new_hostname[0])
			rc |= uci_set_value(ctx, UCI_SYSTEM, UCI_SECTION, "hostname", new_hostname);
		if (new_timezone[0])
			rc |= uci_set_value(ctx, UCI_SYSTEM, UCI_SECTION, "timezone", new_timezone);
		if (rc == 0) {
			strncpy(action_msg, "Settings saved to /etc/config/system.",
				sizeof(action_msg) - 1);
			strncpy(action_type, "success", sizeof(action_type) - 1);
		} else {
			strncpy(action_msg, "Failed to write UCI configuration.",
				sizeof(action_msg) - 1);
			strncpy(action_type, "danger", sizeof(action_type) - 1);
		}
	}

	/* --- read current values --- */
	char cur_hostname[128] = "OpenWrt";
	char cur_timezone[64] = "UTC";
	const char *v;
	v = uci_get_value(ctx, UCI_SYSTEM, UCI_SECTION, "hostname",
			  cur_hostname, sizeof(cur_hostname));
	if (v) strncpy(cur_hostname, v, sizeof(cur_hostname) - 1);
	v = uci_get_value(ctx, UCI_SYSTEM, UCI_SECTION, "timezone",
			  cur_timezone, sizeof(cur_timezone));
	if (v) strncpy(cur_timezone, v, sizeof(cur_timezone) - 1);

	uci_free_context(ctx);

	/* --- render HTML --- */
	printf("Content-Type: text/html; charset=utf-8\n\n");
	printf("<!DOCTYPE html>\n");
	printf("<html lang=\"en\">\n<head>\n");
	printf("<meta charset=\"utf-8\">\n");
	printf("<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">\n");
	printf("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
	printf("<title>OpenWrt &middot; Admin</title>\n");
	printf("<link rel=\"stylesheet\" href=\"../bootstrap/css/bootstrap.min.css\">\n");
	printf("<link rel=\"stylesheet\" href=\"../bootstrap/css/bootstrap-theme.min.css\">\n");
	printf("<style>\n");
	printf("body { padding-top: 70px; }\n");
	printf(".navbar-brand { padding-top: 12px; padding-bottom: 12px; font-size: 18px; }\n");
	printf(".navbar-brand small { font-size: 12px; color: #9d9d9d; }\n");
	printf(".stat { margin-bottom: 20px; min-height: 110px; }\n");
	printf(".stat .value { font-size: 26px; font-weight: bold; margin-top: 6px; }\n");
	printf(".stat .label { font-size: 12px; text-transform: uppercase; letter-spacing: 0.5px; color: #777; margin-top: 2px; }\n");
	printf(".panel-action .btn { min-width: 160px; margin-bottom: 8px; }\n");
	printf("footer { margin-top: 40px; padding-top: 20px; padding-bottom: 40px; border-top: 1px solid #eee; color: #777; font-size: 12px; }\n");
	printf("</style>\n");
	printf("</head>\n<body>\n");

	printf("<nav class=\"navbar navbar-default navbar-fixed-top\">\n");
	printf("  <div class=\"container\">\n");
	printf("    <div class=\"navbar-header\">\n");
	printf("      <a class=\"navbar-brand\" href=\"/cgi-bin/index\">OpenWrt <small>Administrator</small></a>\n");
	printf("    </div>\n");
	printf("  </div>\n");
	printf("</nav>\n");

	printf("<div class=\"container\">\n");

	if (action_msg[0]) {
		printf("<div class=\"alert alert-%s\">%s</div>\n", action_type, action_msg);
	}

	/* Dashboard / system settings */
	printf("<div class=\"page-header\">\n");
	printf("<h1>System <small>Configuration</small></h1>\n");
	printf("</div>\n");

	printf("<form method=\"post\" action=\"/cgi-bin/index\">\n");
	printf("<div class=\"panel panel-default\">\n");
	printf("<div class=\"panel-heading><h3 class=\"panel-title\">General settings</h3></div>\n");
	printf("<div class=\"panel-body\">\n");
	printf("<div class=\"form-group\">\n");
	printf("<label class=\"col-sm-3 control-label\">Hostname</label>\n");
	printf("<div class=\"col-sm-9\">\n");
	printf("<input type=\"text\" class=\"form-control\" name=\"hostname\" value=\"");
	html_escape(cur_hostname, stdout);
	printf("\">\n");
	printf("<p class=\"help-block\">Stored in UCI: system.@system[0].hostname</p>\n");
	printf("</div></div>\n");
	printf("<div class=\"form-group\">\n");
	printf("<label class=\"col-sm-3 control-label\">Timezone</label>\n");
	printf("<div class=\"col-sm-9\">\n");
	printf("<input type=\"text\" class=\"form-control\" name=\"timezone\" value=\"");
	html_escape(cur_timezone, stdout);
	printf("\">\n");
	printf("<p class=\"help-block\">Stored in UCI: system.@system[0].timezone</p>\n");
	printf("</div></div>\n");
	printf("<div class=\"form-group\">\n");
	printf("<div class=\"col-sm-9 col-sm-offset-3\">\n");
	printf("<button type=\"submit\" class=\"btn btn-primary\">Apply changes</button>\n");
	printf(" <span class=\"text-muted\">Writes via libuci + uci_commit</span>\n");
	printf("</div></div>\n");
	printf("</div>\n");
	printf("</div>\n");
	printf("</form>\n");

	/* Current values readout */
	printf("<div class=\"row\">\n");
	printf("<div class=\"col-xs-6 col-md-3\">\n");
	printf("<div class=\"panel panel-default stat\">\n");
	printf("<div class=\"panel-body\">\n");
	printf("<span class=\"label label-default\">Hostname</span>\n");
	printf("<div class=\"value\">");
	html_escape(cur_hostname, stdout);
	printf("</div>\n");
	printf("</div></div></div>\n");
	printf("<div class=\"col-xs-6 col-md-3\">\n");
	printf("<div class=\"panel panel-default stat\">\n");
	printf("<div class=\"panel-body\">\n");
	printf("<span class=\"label label-default\">Timezone</span>\n");
	printf("<div class=\"value\">");
	html_escape(cur_timezone, stdout);
	printf("</div>\n");
	printf("</div></div></div>\n");
	printf("<div class=\"col-xs-6 col-md-3\">\n");
	printf("<div class=\"panel panel-default stat\">\n");
	printf("<div class=\"panel-body\">\n");
	printf("<span class=\"label label-default\">Platform</span>\n");
	printf("<div class=\"value\">brcm-2.4</div>\n");
	printf("<div style=\"font-size:12px;color:#777;margin-top:2px;\">MIPS32 &middot; uClibc</div>\n");
	printf("</div></div></div>\n");
	printf("<div class=\"col-xs-6 col-md-3\">\n");
	printf("<div class=\"panel panel-default stat\">\n");
	printf("<div class=\"panel-body\">\n");
	printf("<span class=\"label label-default\">Config backend</span>\n");
	printf("<div class=\"value\">libuci</div>\n");
	printf("<div style=\"font-size:12px;color:#777;margin-top:2px;\">/etc/config/system</div>\n");
	printf("</div></div></div>\n");
	printf("</div>\n");

	/* Quick actions */
	printf("<div class=\"page-header\">\n");
	printf("<h1>Quick actions <small>System maintenance</small></h1>\n");
	printf("</div>\n");
	printf("<div class=\"panel panel-warning panel-action\">\n");
	printf("<div class=\"panel-heading><h3 class=\"panel-title\">Danger zone</h3></div>\n");
	printf("<div class=\"panel-body\">\n");
	printf("<p class=\"margin-bottom\">Reboot / poweroff require a CGI backend to call the router.</p>\n");
	printf("<button type=\"button\" class=\"btn btn-default btn-block margin-bottom\" disabled>Reboot (backend pending)</button>\n");
	printf("<button type=\"button\" class=\"btn btn-default btn-block\" disabled>Power off (backend pending)</button>\n");
	printf("<p class=\"text-muted\">These call <code>cgi-bin/system?action=…</code>; implement a CGI handler to enable them.</p>\n");
	printf("</div></div>\n");

	printf("<footer>\n");
	printf("<p>OpenWrt administrator control panel &middot; CGI in C, libuci backend &middot; served by busybox httpd &middot; UI: Bootstrap 3.</p>\n");
	printf("</footer>\n");

	printf("</div>\n"); /* container */
	printf("</body>\n</html>\n");

	free(postbody);
	return 0;
}
