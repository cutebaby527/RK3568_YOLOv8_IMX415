#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)

sudo cp "${PROJECT_ROOT}/service/edge-person-monitor.service" /etc/systemd/system/
sudo cp "${PROJECT_ROOT}/service/edge-person-web.service" /etc/systemd/system/

sudo systemctl daemon-reload

sudo systemctl enable edge-person-monitor.service
sudo systemctl enable edge-person-web.service

echo "Services installed."
echo
echo "Start:"
echo "  sudo systemctl start edge-person-monitor"
echo "  sudo systemctl start edge-person-web"
echo
echo "Status:"
echo "  systemctl status edge-person-monitor --no-pager"
echo "  systemctl status edge-person-web --no-pager"
echo
echo "Logs:"
echo "  journalctl -u edge-person-monitor -f"
echo "  journalctl -u edge-person-web -f"
