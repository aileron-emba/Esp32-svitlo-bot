#!/usr/bin/env bash
# Встановлення / оновлення сервера «Світло-бот» на Ubuntu/Debian (запускати від root).
#   bash deploy.sh <BOT_TOKEN> <API_KEY>
# Повторний запуск без аргументів — оновлює код, зберігаючи існуючий .env.
set -euo pipefail

REPO=https://github.com/aileron-emba/esp32-svitlo-bot.git
BRANCH=claude/light-monitor-telegram-na5t5e
DIR=/opt/svitlo

apt-get update -qq && apt-get install -y -qq git python3 curl >/dev/null
id svitlo &>/dev/null || useradd -r -s /usr/sbin/nologin svitlo

if [ -d "$DIR/.git" ]; then
  git -c safe.directory="$DIR" -C "$DIR" fetch -q origin "$BRANCH"
  git -c safe.directory="$DIR" -C "$DIR" reset -q --hard "origin/$BRANCH"
else
  git clone -q -b "$BRANCH" "$REPO" "$DIR"
fi

ENV="$DIR/server/.env"
if [ $# -ge 2 ]; then
  cat > "$ENV" <<EOF
BOT_TOKEN=$1
API_KEY=$2
PORT=8080
OFFLINE_TIMEOUT=90
DB_PATH=$DIR/server/svitlo.db
TIMEZONE=Europe/Kyiv
EOF
elif [ ! -f "$ENV" ]; then
  echo "Перший запуск: bash deploy.sh <BOT_TOKEN> <API_KEY>" >&2
  exit 1
fi

chown -R svitlo "$DIR" && chmod 600 "$ENV"
cp "$DIR/server/svitlo.service" /etc/systemd/system/svitlo.service
systemctl daemon-reload
systemctl enable -q svitlo
systemctl restart svitlo
command -v ufw >/dev/null && ufw status | grep -q active && ufw allow 8080/tcp >/dev/null || true

sleep 2
curl -fsS localhost:8080/health && echo && echo "Готово. Лог: journalctl -u svitlo -f"
