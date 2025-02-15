# git pull && make clean && make magpie BUILD=release
if [ "$2" == "" ]; then
    ./download_data.sh
    ./bin/magpie createdata klv $1 english
    ./bin/magpie convert klv2csv $1 $1 -ld english -lex $1
    ./convert_lexica.sh
else 
    ./bin/magpie leavegen $2 $3 -lex $1 -leaves $1 -hr true -threads 32 -pfreq 100000 -wmp true
fi