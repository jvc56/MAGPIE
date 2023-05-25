package main

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unicode"

	"github.com/domino14/macondo/alphabet"
	"github.com/domino14/macondo/gaddagmaker"
)

const (
	KLVMagicNumber                = "cldg"
	LetterDistributionMagicNumber = "clds"
	LetterConversionMagicNumber   = "clcv"
	InputDataDirectory            = "data"
	LetterDistributionsDirectory  = "letterdistributions"
	LexicaDirectory               = "lexica"
	OutputDataDirectory           = "../core/data"
	NumberOfUniqueMachineLetters  = alphabet.MaxAlphabetSize + 1
	NumberOfUniqueEnglishTiles    = 27
	BlankMask                     = 0x80
	UnblankMask                   = (0x80 - 1)
)

// Internal struct only used to write klv to file
type KLV struct {
	edges         []uint32
	values        []float64
	leavesToIndex map[string]int
	alphabet      *alphabet.Alphabet
	invalidIndex  uint32
}

func getLexiconFilename(lexicon string) string {
	return filepath.Join(InputDataDirectory, LexicaDirectory, lexicon+".txt")
}

func getLeavesFilename(lexicon string) string {
	return filepath.Join(InputDataDirectory, LexicaDirectory, lexicon+".csv")
}

func getLetterDistributionFilename(letterDistribution string) string {
	return filepath.Join(InputDataDirectory, LetterDistributionsDirectory, letterDistribution+".csv")
}

func getGaddagOutputFilename(lexicon string) string {
	return filepath.Join(OutputDataDirectory, LexicaDirectory, lexicon+".gaddag")
}

func getKLVOutputFilename(lexicon string) string {
	return filepath.Join(OutputDataDirectory, LexicaDirectory, lexicon+".klv2")
}

func getLetterDistributionOutputFilename(letterDistribution string) string {
	return filepath.Join(OutputDataDirectory, LetterDistributionsDirectory, letterDistribution+".dist")
}

func getLetterConversionOutputFilename(lexicon string) string {
	return filepath.Join(OutputDataDirectory, LexicaDirectory, lexicon+".alph")
}

func createGaddag(lexicon string) *gaddagmaker.Gaddag {
	gaddag := gaddagmaker.GenerateGaddag(getLexiconFilename(lexicon), true, true)
	err := os.Rename("out.gaddag", getGaddagOutputFilename(lexicon))
	if err != nil {
		panic(err)
	}
	return gaddag
}

func readLeaves(filePath string, alph *alphabet.Alphabet) ([]float64, []string, map[string]int) {
	file, err := os.Open(filePath)
	if err != nil {
		panic(err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	leavesToIndex := map[string]int{}
	values := []float64{}
	leaves := []string{}
	index := 0
	for scanner.Scan() {
		line := scanner.Text()
		leaveAndValue := strings.Split(line, ",")
		leave := alphabet.RackFromString(strings.TrimSpace(leaveAndValue[0]), alph).String()
		value, err := strconv.ParseFloat(strings.TrimSpace(leaveAndValue[1]), 64)
		if err != nil {
			panic(err)
		}
		values = append(values, value)
		leaves = append(leaves, leave)
		// We add 1 here because the empty leave
		// will be inserted at the beginning later.
		leavesToIndex[leave] = index + 1
		index++
	}

	if err := scanner.Err(); err != nil {
		panic(err)
	}
	return values, leaves, leavesToIndex
}

func convertNodeIndexToFirstEdgeIndex(index int, takeAddArrayLength int) int {
	return index * takeAddArrayLength * 2
}

func clearAdd(klv *KLV, index int, takeAddArrayLength int) {
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index, takeAddArrayLength)
	for i := 0; i < takeAddArrayLength; i++ {
		klv.edges[firstEdgeIndex+i] = klv.invalidIndex
	}
}

func populateAdd(klv *KLV, rack string, index int, takeAddArrayLength int) {
	if len(rack) >= 6 {
		clearAdd(klv, index, takeAddArrayLength)
	} else {
		mlRack := alphabet.RackFromString(rack, klv.alphabet)
		firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index, takeAddArrayLength)
		for i := 0; i < takeAddArrayLength; i++ {
			thisEdgeIndex := firstEdgeIndex + i
			// Blank is still some arbitrary number greater than 0
			// Set the blank to this value.
			tileVal := i
			if i == 0 {
				tileVal = 50
			} else {
				tileVal = i - 1
			}
			ml := alphabet.MachineLetter(tileVal)
			mlRack.Add(ml)
			addedRackString := mlRack.String()
			leaveIndex, exists := klv.leavesToIndex[addedRackString]
			if exists {
				klv.edges[thisEdgeIndex] = uint32(leaveIndex)
			} else {
				klv.edges[thisEdgeIndex] = klv.invalidIndex
			}
			mlRack.Take(ml)
		}
	}
}

func clearTake(klv *KLV, index int, takeAddArrayLength int) {
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index, takeAddArrayLength)
	for i := 0; i < takeAddArrayLength; i++ {
		klv.edges[firstEdgeIndex+takeAddArrayLength+i] = klv.invalidIndex
	}
}

func populateTake(klv *KLV, rack string, index int, takeAddArrayLength int) {
	mlRack := alphabet.RackFromString(rack, klv.alphabet)
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index, takeAddArrayLength)
	for i := 0; i < takeAddArrayLength; i++ {
		tileVal := i
		if i == 0 {
			tileVal = 50
		} else {
			tileVal = i - 1
		}
		ml := alphabet.MachineLetter(tileVal)
		thisEdgeIndex := firstEdgeIndex + takeAddArrayLength + i
		if mlRack.Has(ml) {
			mlRack.Take(ml)
			takenRackString := mlRack.String()
			klv.edges[thisEdgeIndex] = uint32(klv.leavesToIndex[takenRackString])
			mlRack.Add(ml)
		} else {
			klv.edges[thisEdgeIndex] = klv.invalidIndex
		}
	}
}

func createKLV(lexicon string, alph *alphabet.Alphabet) {
	values, leaves, leavesToIndex := readLeaves(getLeavesFilename(lexicon), alph)

	takeAddArrayLength := int(alph.NumLetters() + 1)
	fmt.Printf("takeAddArrayLength: %d\n", takeAddArrayLength)
	// Use max alphabet size + 1 so there is room for the blank
	// Add 2 extra nodes, one for the empty leave and one for
	// the full rack starting leave.
	klv := &KLV{}
	numberOfNodes := len(leaves) + 2
	numberOfEdges := convertNodeIndexToFirstEdgeIndex(numberOfNodes, takeAddArrayLength)
	klvEdges := make([]uint32, numberOfEdges)
	klvValues := make([]float64, numberOfNodes)

	klv.edges = klvEdges
	klv.values = klvValues
	klv.leavesToIndex = leavesToIndex
	klv.invalidIndex = uint32(numberOfNodes)
	klv.alphabet = alph

	populateAdd(klv, "", 0, takeAddArrayLength)
	clearTake(klv, 0, takeAddArrayLength)
	klv.values[0] = 0

	for i := 1; i < numberOfNodes-1; i++ {
		klv.values[i] = values[i-1]
		populateAdd(klv, leaves[i-1], i, takeAddArrayLength)
		populateTake(klv, leaves[i-1], i, takeAddArrayLength)
	}

	klv.values[numberOfNodes-1] = 0
	clearAdd(klv, numberOfNodes-1, takeAddArrayLength)
	clearTake(klv, numberOfNodes-1, takeAddArrayLength)

	saveKLV(klv, lexicon)
}

func float64ToByte(f float64) []byte {
	var buf [8]byte
	binary.BigEndian.PutUint64(buf[:], math.Float64bits(f))
	return buf[:]
}

func saveKLV(klv *KLV, lexicon string) {
	file, err := os.Create(getKLVOutputFilename(lexicon))
	if err != nil {
		panic(err)
	}
	// Write the magic number
	file.WriteString(KLVMagicNumber)

	// Write the lexicon name
	bts := []byte(lexicon)
	binary.Write(file, binary.BigEndian, uint8(len(bts)))
	binary.Write(file, binary.BigEndian, bts)

	// Write the number of values
	binary.Write(file, binary.BigEndian, uint32(len(klv.values)))

	// Write the edges, these are already serialized
	binary.Write(file, binary.BigEndian, klv.edges)

	// Serialize the values
	serializedValues := []byte{}
	for i := 0; i < len(klv.values); i++ {
		serializedValues = append(serializedValues, float64ToByte(klv.values[i])...)
	}

	// Write the values
	binary.Write(file, binary.BigEndian, serializedValues)

	file.Close()
}

func loadLetterDistribution(letterDistribution string) *alphabet.LetterDistribution {
	filename := getLetterDistributionFilename(letterDistribution)

	file, err := os.Open(filename)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	ld, err := alphabet.ScanLetterDistribution(file)
	if err != nil {
		panic(err)
	}
	return ld
}

func createLetterDistribution(letterDistribution string) {
	ld := loadLetterDistribution(letterDistribution)
	saveLetterDistribution(ld, letterDistribution)
}

func saveLetterDistribution(ld *alphabet.LetterDistribution, ldName string) {
	file, err := os.Create(getLetterDistributionOutputFilename(ldName))
	if err != nil {
		panic(err)
	}

	// Serialize the letter distribution
	distSize := len(ld.SortOrder)
	dist := make([]uint32, distSize)
	pointValues := make([]uint32, distSize)
	isVowel := make([]uint32, distSize)

	isVowelMap := map[rune]bool{}
	for _, vowel := range ld.Vowels {
		isVowelMap[vowel] = true
	}

	for runeLetter := range ld.SortOrder {
		ml, err := ld.Alphabet().Val(runeLetter)
		if err != nil {
			panic(err)
		}
		mlAsIndex := uint32(ml)
		if mlAsIndex == 50 {
			mlAsIndex = 0
		} else {
			mlAsIndex++
		}
		dist[mlAsIndex] = uint32(ld.Distribution[runeLetter])
		pointValues[mlAsIndex] = uint32(ld.PointValues[runeLetter])
		if isVowelMap[runeLetter] {
			isVowel[mlAsIndex] = 1
		} else {
			isVowel[mlAsIndex] = 0
		}
	}

	// Write the magic number
	file.WriteString(LetterDistributionMagicNumber)

	// Write the letter distribution name
	bts := []byte(ldName)
	binary.Write(file, binary.BigEndian, uint8(len(bts)))
	binary.Write(file, binary.BigEndian, bts)

	// We do not write the lengths because they are
	// constants across all programs that deal with them

	// Write the size of the distribution
	binary.Write(file, binary.BigEndian, uint32(distSize))

	// Write the distribution
	binary.Write(file, binary.BigEndian, dist)

	// Write the point values
	binary.Write(file, binary.BigEndian, pointValues)

	// Write the vowels boolean array
	binary.Write(file, binary.BigEndian, isVowel)

	file.Close()
}

func zeroBlankVal(alph *alphabet.Alphabet, r rune) (alphabet.MachineLetter, error) {
	if r == alphabet.BlankToken {
		return 0, nil
	}
	val, ok := alph.Vals()[r]
	val++
	if ok {
		return val, nil
	}
	if r == unicode.ToLower(r) {
		val, ok = alph.Vals()[unicode.ToUpper(r)]
		val++
		if ok {
			return (val | BlankMask), nil
		}
	}
	if r == alphabet.ASCIIPlayedThrough {
		return 0, nil
	}
	return 0, fmt.Errorf("letter `%c` not found in alphabet", r)
}

func zeroBlankUserVisible(alph *alphabet.Alphabet, ml alphabet.MachineLetter) rune {
	if ml == 0 {
		return alphabet.BlankToken
	}
	ml--
	if (ml & BlankMask) > 0 {
		return unicode.ToLower(alph.Letters()[ml&UnblankMask])
	}
	return alph.Letters()[ml]
}

func createLetterConversion(lexicon string, alph *alphabet.Alphabet) {
	saveLetterConversion(lexicon, alph)
}

func saveLetterConversion(lexicon string, alph *alphabet.Alphabet) {
	file, err := os.Create(getLetterConversionOutputFilename(lexicon))
	if err != nil {
		panic(err)
	}

	// Write the magic number
	file.WriteString(LetterConversionMagicNumber)

	// Write the lexicon name
	bts := []byte(lexicon)
	binary.Write(file, binary.BigEndian, uint8(len(bts)))
	binary.Write(file, binary.BigEndian, bts)

	// Machine letters range from 0..255
	machineLetterMaxValue := 256
	userVisibleLetters := make([]uint32, 256)
	for i := 0; i < machineLetterMaxValue; i++ {
		ml := alphabet.MachineLetter(i)
		userVisibleLetters[i] = uint32(zeroBlankUserVisible(alph, ml))
	}

	// Write the machine letter -> user visible map
	binary.Write(file, binary.BigEndian, uint32(machineLetterMaxValue))
	binary.Write(file, binary.BigEndian, userVisibleLetters)

	// This is just some large number to cover all of the potential
	// rune values. This should be sufficient for all languages.
	userVisibleLetterMaxValue := 10000
	invalidMachineLetterValue := machineLetterMaxValue
	machineLetters := make([]uint32, userVisibleLetterMaxValue)
	for i := 0; i < userVisibleLetterMaxValue; i++ {
		thisRune := rune(i)
		val, err := zeroBlankVal(alph, thisRune)
		if err != nil {
			machineLetters[i] = uint32(invalidMachineLetterValue)
		}
		machineLetters[i] = uint32(val)
	}

	binary.Write(file, binary.BigEndian, uint32(userVisibleLetterMaxValue))
	binary.Write(file, binary.BigEndian, machineLetters)
}

func main() {
	if len(os.Args) != 3 {
		panic("must specify lexicon and letter distribution")
	}
	lexicon := os.Args[1]
	letterDistribution := os.Args[2]

	gaddag := createGaddag(lexicon)
	createKLV(lexicon, gaddag.Alphabet)
	createLetterDistribution(letterDistribution)
	createLetterConversion(lexicon, gaddag.Alphabet)
}
