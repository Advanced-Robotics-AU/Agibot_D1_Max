export RMW_IMPLEMENTATION=rmw_zenoh_cpp
export ROBOT_IP=${ROBOT_IP:-10.220.1.3}

# 1. Match the robot's Domain ID (default is 0)

if [ "$1" = "rk" ]; then
    export ROS_DOMAIN_ID=66
elif [ "$1" = "nx" ]; then
    export ROS_DOMAIN_ID=24
else
    echo "Please provide argument for pc, rk(ROCK chip) or nx (ORIN)"
    echo "Usage: ./env.sh rk or ./env.sh nx"

    return 1 2>/dev/null || exit 1
fi

# 2. Point the client to the robot's Zenoh router port (7447)
export ZENOH_CONFIG_OVERRIDE='mode="client";connect/endpoints=["tcp/'"$ROBOT_IP"':7447"]'