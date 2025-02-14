# git pull && make clean && make magpie BUILD=release
# ./bin/magpie convert klv2csv CSW24_gen_ CSW24_gen_ -ld english -lex CSW24
if [ "$1" == "c" ]; then
    ./download_data.sh
    ./bin/magpie createdata klv CSW24 english
    ./convert_lexica.sh
else 
    ./bin/magpie leavegen $1 $2 -lex CSW24 -leaves CSW24 -hr true -threads 32 -pfreq 100000 -wmp true
fi