#!/bin/sh
#
# OpenWrt minimalist administrator control panel
# Shell: ash (busybox)  ·  UI: Bootstrap 3  ·  Config: UCI (/sbin/uci)
# Installed at: www/cgi-bin/index   (executed by busybox httpd)
#

# --- CGI environment ---
REQUEST_METHOD="${REQUEST_METHOD:-GET}"
QUERY_STRING="${QUERY_STRING:-}"
CONTENT_LENGTH="${CONTENT_LENGTH:-0}"

# --- helpers ---

html_escape() {
    # Escape & < > " for safe HTML insertion
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g'
}

url_decode() {
    # Decode + and %XX in a URL-encoded string
    local s
    s=$(printf '%s\n' "$1" | sed 's/+/ /g; s/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')
    printf '%b' "$s"
}

# get_form <key> : print the first matching value from the form data ($FORM)
get_form() {
    local key="$1" pair k v rest="$FORM"
    while [ -n "$rest" ]; do
        case "$rest" in
            *\&*)
                pair="${rest%%\&*}"
                rest="${rest#*\&}"
                ;;
            *)
                pair="$rest"
                rest=""
                ;;
        esac
        case "$pair" in
            *=*)
                k="${pair%%=*}"
                v="${pair#*=}"
                ;;
            *)
                k="$pair"
                v=""
                ;;
        esac
        k=$(url_decode "$k")
        v=$(url_decode "$v")
        if [ "$k" = "$key" ]; then
            printf '%s' "$v"
            return 0
        fi
    done
    return 1
}

# uci_get <key> <default> : read a UCI value, fall back to default
uci_get() {
    local key="$1" def="$2" val
    val=$(/sbin/uci get "$key" 2>/dev/null)
    if [ $? -eq 0 ] && [ -n "$val" ]; then
        printf '%s' "$val"
    else
        printf '%s' "$def"
    fi
}

# --- read request body (POST) or query string (GET) ---
FORM=""
if [ "$REQUEST_METHOD" = "POST" ]; then
    if [ -n "$CONTENT_LENGTH" ] && [ "$CONTENT_LENGTH" -gt 0 ] 2>/dev/null; then
        FORM=$(head -c "$CONTENT_LENGTH")
    fi
elif [ -n "$QUERY_STRING" ]; then
    FORM="$QUERY_STRING"
fi

# --- handle form submission (save settings) ---
SAVE_MSG=""
SAVE_CLASS="info"
if [ "$(get_form save)" = "1" ]; then
    new_hostname=$(get_form hostname)
    new_timezone=$(get_form timezone)
    ok=1
    if [ -n "$new_hostname" ]; then
        /sbin/uci set "system.@system[0].hostname=$new_hostname" >/dev/null 2>&1 || ok=0
    fi
    if [ -n "$new_timezone" ]; then
        /sbin/uci set "system.@system[0].timezone=$new_timezone" >/dev/null 2>&1 || ok=0
    fi
    if [ $ok -eq 1 ]; then
        if /sbin/uci commit system >/dev/null 2>&1; then
            SAVE_MSG="Settings saved to /etc/config/system."
            SAVE_CLASS="success"
        else
            SAVE_MSG="Failed to commit UCI configuration."
            SAVE_CLASS="danger"
        fi
    else
        SAVE_MSG="Failed to update UCI configuration."
        SAVE_CLASS="danger"
    fi
fi

# --- read current values ---
hostname=$(uci_get "system.@system[0].hostname" "OpenWrt")
timezone=$(uci_get "system.@system[0].timezone" "UTC")

# --- system info ---
uptime_val=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo "-")
load_avg=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo "-")

# --- HTTP headers ---
printf 'Content-Type: text/html; charset=utf-8\r\n\r\n'

# --- HTML page ---
cat <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenWrt &middot; Admin</title>
<link rel="stylesheet" href="../bootstrap/css/bootstrap.min.css">
<link rel="stylesheet" href="../bootstrap/css/bootstrap-theme.min.css">
<style>
body { padding-top: 70px; }
.navbar-brand { padding-top: 12px; padding-bottom: 12px; font-size: 18px; }
.navbar-brand small { font-size: 12px; color: #9d9d9d; }
.stat { margin-bottom: 20px; min-height: 110px; }
.stat .value { font-size: 26px; font-weight: bold; margin-top: 6px; }
.stat .label { font-size: 12px; text-transform: uppercase; letter-spacing: 0.5px; color: #777; margin-top: 2px; }
.panel-action .btn { min-width: 160px; margin-bottom: 8px; }
footer { margin-top: 40px; padding-top: 20px; padding-bottom: 40px; border-top: 1px solid #eee; color: #777; font-size: 12px; }
</style>
</head>
<body>

<nav class="navbar navbar-default navbar-fixed-top">
  <div class="container">
    <div class="navbar-header">
      <a class="navbar-brand" href="/cgi-bin/index">OpenWrt <small>Administrator</small></a>
    </div>
  </div>
</nav>

<div class="container">
EOF

if [ -n "$SAVE_MSG" ]; then
    printf '<div class="alert alert-%s">%s</div>\n' "$SAVE_CLASS" "$(html_escape "$SAVE_MSG")"
fi

cat <<EOF
<div class="page-header">
  <h1>System <small>Configuration</small></h1>
</div>

<form method="post" action="/cgi-bin/index">
<div class="panel panel-default">
  <div class="panel-heading"><h3 class="panel-title">General settings</h3></div>
  <div class="panel-body">
    <div class="form-group">
      <label class="col-sm-3 control-label">Hostname</label>
      <div class="col-sm-9">
        <input type="text" class="form-control" name="hostname" value="$(html_escape "$hostname")">
        <p class="help-block">UCI: system.@system[0].hostname</p>
      </div>
    </div>
    <div class="form-group">
      <label class="col-sm-3 control-label">Timezone</label>
      <div class="col-sm-9">
        <input type="text" class="form-control" name="timezone" value="$(html_escape "$timezone")">
        <p class="help-block">UCI: system.@system[0].timezone</p>
      </div>
    </div>
    <div class="form-group">
      <div class="col-sm-9 col-sm-offset-3">
        <button type="submit" name="save" value="1" class="btn btn-primary">Apply changes</button>
        <span class="text-muted"> Writes via /sbin/uci + uci commit</span>
      </div>
    </div>
  </div>
</div>
</form>

<div class="row">
  <div class="col-xs-6 col-md-3">
    <div class="panel panel-default stat">
      <div class="panel-body">
        <span class="label label-default">Hostname</span>
        <div class="value">$(html_escape "$hostname")</div>
      </div>
    </div>
  </div>
  <div class="col-xs-6 col-md-3">
    <div class="panel panel-default stat">
      <div class="panel-body">
        <span class="label label-default">Timezone</span>
        <div class="value">$(html_escape "$timezone")</div>
      </div>
    </div>
  </div>
  <div class="col-xs-6 col-md-3">
    <div class="panel panel-default stat">
      <div class="panel-body">
        <span class="label label-default">Uptime</span>
        <div class="value">$(html_escape "$uptime_val") s</div>
      </div>
    </div>
  </div>
  <div class="col-xs-6 col-md-3">
    <div class="panel panel-default stat">
      <div class="panel-body">
        <span class="label label-default">Load average</span>
        <div class="value">$(html_escape "$load_avg")</div>
      </div>
    </div>
  </div>
</div>

<div class="page-header">
  <h1>Quick actions <small>System maintenance</small></h1>
</div>
<div class="panel panel-warning panel-action">
  <div class="panel-heading"><h3 class="panel-title">Danger zone</h3></div>
  <div class="panel-body">
    <p class="margin-bottom">Reboot / poweroff need a CGI backend handler to call the router.</p>
    <button type="button" class="btn btn-default btn-block margin-bottom" disabled>Reboot (backend pending)</button>
    <button type="button" class="btn btn-default btn-block" disabled>Power off (backend pending)</button>
    <p class="text-muted">Implement a <code>cgi-bin/system</code> handler to enable these.</p>
  </div>
</div>

<footer>
  <p>OpenWrt admin control panel &middot; ash CGI &middot; UCI backend &middot; busybox httpd &middot; Bootstrap 3.</p>
</footer>

</div>
</body>
</html>
EOF
