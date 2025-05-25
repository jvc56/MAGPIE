curl https://codeload.github.com/jvc56/MAGPIE-DATA/tar.gz/main | tar -xz --strip=1 MAGPIE-DATA-main/
for file in testdata/leaves/*.gz; do gunzip -f "$file"; done