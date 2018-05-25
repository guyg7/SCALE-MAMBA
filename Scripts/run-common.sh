
run_player() {
    bin=$1
    shift
    if ! test -e Scripts/logs; then
        mkdir Scripts/logs
    fi
    params="$*"
    rem=$(($players - 2))
    for i in $(seq 0 $rem); do
      echo "trying with player $i"
      >&2 echo Running ../$bin $i $params
      ./$bin $i $params 2>&1 | tee Scripts/logs/$i  &
    done
    last_player=$(($players - 1))
    >&2 echo Running ../$bin $last_player $params
    ./$bin $last_player $params > Scripts/logs/$last_player 2>&1 || return 1
}

killall Player.x 
sleep 0.5

declare -i players=$(sed -n 2p Data/NetworkData.txt)

#. Scripts/setup.sh
