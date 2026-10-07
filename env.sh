#!/usr/bin/env bash
# Usage: source env.sh <rk|nx> [robot_ip]
#   e.g. source env.sh rk
#        source env.sh nx 192.168.1.50

export RMW_IMPLEMENTATION=rmw_zenoh_cpp

# 1. Match the robot's Domain ID
if [ "$1" = "rk" ]; then
    export ROS_DOMAIN_ID=66
elif [ "$1" = "nx" ]; then
    export ROS_DOMAIN_ID=24
else
    echo "Please provide an argument: rk (Rockchip) or nx (Orin), plus an optional robot IP"
    echo "Usage: source env.sh <rk|nx> [robot_ip]"
    return 1 2>/dev/null || exit 1
fi

# 2. Robot IP: second argument, or the default if none is given
export ROBOT_IP="${2:-10.220.1.2}"

# 3. Point the client to the robot's Zenoh router (port 7447)
export ZENOH_CONFIG_OVERRIDE='mode="client";connect/endpoints=["tcp/'"$ROBOT_IP"':7447"]'

echo "RMW=$RMW_IMPLEMENTATION  DOMAIN=$ROS_DOMAIN_ID  ROBOT=$ROBOT_IP:7447"
