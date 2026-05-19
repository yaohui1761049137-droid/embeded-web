#!/bin/bash
# deploy.sh - Deploy Boa server and web files to LubanCat board
# Usage: ./deploy.sh [board_ip]
# Default IP: 192.168.8.1

BOARD_IP="${1:-192.168.8.1}"
BOARD_USER="root"
BOARD_DIR="/home/yao/embeded_web/package"

echo "=== Deploying to LubanCat at ${BOARD_IP} ==="

# Check connectivity
ping -c 1 -W 2 ${BOARD_IP} > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "ERROR: Cannot reach ${BOARD_IP}. Check network connection."
    exit 1
fi

echo "Creating directories on board..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p /home/www/cgi-bin /var/log/boa /etc /tmp/boa_sessions"

echo "Copying boa binary..."
scp ${BOARD_DIR}/bin/boa ${BOARD_USER}@${BOARD_IP}:/usr/sbin/

echo "Copying web files..."
scp ${BOARD_DIR}/www/* ${BOARD_USER}@${BOARD_IP}:/home/www/

echo "Copying CGI binaries..."
scp ${BOARD_DIR}/cgi-bin/* ${BOARD_USER}@${BOARD_IP}:/home/www/cgi-bin/

echo "Copying configuration..."
scp ${BOARD_DIR}/etc/boa.conf ${BOARD_USER}@${BOARD_IP}:/etc/
scp ${BOARD_DIR}/etc/mime.types ${BOARD_USER}@${BOARD_IP}:/etc/

echo "Setting permissions..."
ssh ${BOARD_USER}@${BOARD_IP} "chmod 755 /usr/sbin/boa && chmod 755 /home/www/cgi-bin/* && chmod 755 /home/www/*.html && chmod 644 /home/www/*.css && chmod 644 /etc/boa.conf && chmod 644 /etc/mime.types"

echo ""
echo "=== Deployment complete ==="
echo "Start boa on board: boa -c /etc/"
echo "Access web:  http://${BOARD_IP}/"
echo "Default login: admin / admin"
