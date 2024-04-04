SRC_PATH="https://github.com/jvc56/MAGPIE-DATA/raw/main"
SRC_LEXICA_PATH="$SRC_PATH/lexica"
DST_LEXICA_PATH="data/lexica"
SRC_STRATEGY_PATH="$SRC_PATH/strategy"
DST_STRATEGY_PATH="data/strategy"
SRC_LAYOUTS_PATH="$SRC_PATH/layouts"
DST_LAYOUTS_PATH="data/layouts"
mkdir -p "$DST_LEXICA_PATH" &&
mkdir -p "$DST_STRATEGY_PATH" &&
mkdir -p "$DST_LAYOUTS_PATH" &&

lexicons=("CSW21" "NWL20" "DISC2" "FRA20" "OSPS49" "CEL_super")
data_types=("kwg" "klv2")

for lexicon in "${lexicons[@]}"; do
    for data_type in "${data_types[@]}"; do
        wget_output=$(wget -O "$DST_LEXICA_PATH/$lexicon.$data_type" "$SRC_LEXICA_PATH/$lexicon.$data_type")
        if [ $? -ne 0 ]; then
            echo "Failed to download $SRC_LEXICA_PATH/$lexicon.$data_type: $wget_output"
            exit 1
        fi
    done
done

csw_leaves="CSW21.csv.gz"

# Download the CSW leaves csv
wget -O "$DST_LEXICA_PATH/$csw_leaves" "$SRC_LEXICA_PATH/$csw_leaves" 
yes | gunzip "$DST_LEXICA_PATH/$csw_leaves"

# Download the win percentages

wget -O "$DST_STRATEGY_PATH/winpct.csv" "$SRC_STRATEGY_PATH/winpct.csv" 

# Download the standard layouts

wget -O "$DST_LAYOUTS_PATH/standard15.txt" "$SRC_LAYOUTS_PATH/standard15.txt" 
wget -O "$DST_LAYOUTS_PATH/standard21.txt" "$SRC_LAYOUTS_PATH/standard21.txt" 
