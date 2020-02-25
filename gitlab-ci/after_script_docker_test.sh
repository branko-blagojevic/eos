#!/bin/bash -ve

./eos-docker/scripts/collect_logs.sh eos-logs/
sudo ./eos-docker/scripts/shutdown_services.sh

docker system prune --all --filter until=30m --force # remove unused docker resources