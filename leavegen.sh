
git pull && make clean && make magpie BUILD=release
if [ "$3" != "" ]; then
    ./download_data.sh
    # Create empty klv
    ./bin/magpie createdata klv CSW24 english
    ./convert_lexica.sh
fi
./bin/magpie leavegen $1 $2 -lex CSW24 -leaves CSW24 -hr true -threads 2 -pfreq 100000 -wmp