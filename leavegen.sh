# git pull && make clean && make magpie BUILD=release
if [ "$2" == "" ]; then
    ./download_data.sh
    rm -rf data/lexica/$1.klv2
    ./bin/magpie createdata klv $1 english
    rm -rf data/leaves/$1.csv
    ./bin/magpie convert klv2csv $1 $1 -ld english -lex $1
    ./convert_lexica.sh
else 
    cmd="./bin/magpie leavegen $3 $4 -lex $2 -leaves $1 -hr true -threads 32 -pfreq 100000 -wmp true"
    echo "$cmd"
    eval "$cmd"
fi
