package main

import (
	"bufio"
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/domino14/macondo/alphabet"
	"github.com/domino14/macondo/gaddagmaker"
)

const (
	LaddagMagicNumber             = "cldg"
	LetterDistributionMagicNumber = "clds"
	LetterConversionMagicNumber   = "clcv"
	InputDataDirectory            = "data"
	LetterDistributionsDirectory  = "letterdistributions"
	LexicaDirectory               = "lexica"
	OutputDataDirectory           = "../core/data"
	NumberOfUniqueMachineLetters  = alphabet.MaxAlphabetSize + 1
)

// Internal struct only used to write laddag to file
type Laddag struct {
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

func getLaddagOutputFilename(lexicon string) string {
	return filepath.Join(OutputDataDirectory, LexicaDirectory, lexicon+".laddag")
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

func convertNodeIndexToFirstEdgeIndex(index int) int {
	return index * NumberOfUniqueMachineLetters * 2
}

func clearAdd(laddag *Laddag, index int) {
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index)
	for i := 0; i < NumberOfUniqueMachineLetters; i++ {
		laddag.edges[firstEdgeIndex+i] = laddag.invalidIndex
	}
}

func populateAdd(laddag *Laddag, rack string, index int) {
	if len(rack) >= 6 {
		clearAdd(laddag, index)
	} else {
		mlRack := alphabet.RackFromString(rack, laddag.alphabet)
		firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index)
		for i := 0; i < NumberOfUniqueMachineLetters; i++ {
			thisEdgeIndex := firstEdgeIndex + i
			ml := alphabet.MachineLetter(i)
			mlRack.Add(ml)
			addedRackString := mlRack.String()
			leaveIndex, exists := laddag.leavesToIndex[addedRackString]
			if exists {
				laddag.edges[thisEdgeIndex] = uint32(leaveIndex)
			} else {
				laddag.edges[thisEdgeIndex] = laddag.invalidIndex
			}
			mlRack.Take(ml)
		}
	}
}

func clearTake(laddag *Laddag, index int) {
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index)
	for i := 0; i < NumberOfUniqueMachineLetters; i++ {
		laddag.edges[firstEdgeIndex+NumberOfUniqueMachineLetters+i] = laddag.invalidIndex
	}
}

func populateTake(laddag *Laddag, rack string, index int) {
	mlRack := alphabet.RackFromString(rack, laddag.alphabet)
	firstEdgeIndex := convertNodeIndexToFirstEdgeIndex(index)
	for i := 0; i < NumberOfUniqueMachineLetters; i++ {
		ml := alphabet.MachineLetter(i)
		thisEdgeIndex := firstEdgeIndex + NumberOfUniqueMachineLetters + i
		if mlRack.Has(ml) {
			mlRack.Take(ml)
			takenRackString := mlRack.String()
			laddag.edges[thisEdgeIndex] = uint32(laddag.leavesToIndex[takenRackString])
			mlRack.Add(ml)
		} else {
			laddag.edges[thisEdgeIndex] = laddag.invalidIndex
		}
	}
}
func createLaddag(lexicon string, alph *alphabet.Alphabet) {
	values, leaves, leavesToIndex := readLeaves(getLeavesFilename(lexicon), alph)

	// Use max alphabet size + 1 so there is room for the blank
	// Add 2 extra nodes, one for the empty leave and one for
	// the full rack starting leave.
	laddag := &Laddag{}
	numberOfNodes := len(leaves) + 2
	numberOfEdges := convertNodeIndexToFirstEdgeIndex(numberOfNodes)
	laddagEdges := make([]uint32, numberOfEdges)
	laddagValues := make([]float64, numberOfNodes)

	laddag.edges = laddagEdges
	laddag.values = laddagValues
	laddag.leavesToIndex = leavesToIndex
	laddag.invalidIndex = uint32(numberOfNodes)
	laddag.alphabet = alph

	populateAdd(laddag, "", 0)
	clearTake(laddag, 0)
	laddag.values[0] = 0

	for i := 1; i < numberOfNodes-1; i++ {
		laddag.values[i] = values[i-1]
		populateAdd(laddag, leaves[i-1], i)
		populateTake(laddag, leaves[i-1], i)
	}

	laddag.values[numberOfNodes-1] = 0
	clearAdd(laddag, numberOfNodes-1)
	clearTake(laddag, numberOfNodes-1)

	saveLaddag(laddag, lexicon)
}

func float64ToByte(f float64) []byte {
	var buf [8]byte
	binary.BigEndian.PutUint64(buf[:], math.Float64bits(f))
	return buf[:]
}

func saveLaddag(laddag *Laddag, lexicon string) {
	file, err := os.Create(getLaddagOutputFilename(lexicon))
	if err != nil {
		panic(err)
	}
	// Write the magic number
	file.WriteString(LaddagMagicNumber)

	// Write the lexicon name
	bts := []byte(lexicon)
	binary.Write(file, binary.BigEndian, uint8(len(bts)))
	binary.Write(file, binary.BigEndian, bts)

	// Write the number of values
	binary.Write(file, binary.BigEndian, uint32(len(laddag.values)))

	// Write the edges, these are already serialized
	binary.Write(file, binary.BigEndian, laddag.edges)

	// Serialize the values
	serializedValues := []byte{}
	for i := 0; i < len(laddag.values); i++ {
		serializedValues = append(serializedValues, float64ToByte(laddag.values[i])...)
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
	distSize := alphabet.MaxAlphabetSize + 1
	dist := make([]uint32, distSize)
	pointValues := make([]uint32, distSize)
	isVowel := make([]uint32, distSize)

	isVowelMap := map[rune]bool{}
	for _, vowel := range ld.Vowels {
		isVowelMap[vowel] = true
	}

	for runeLetter, _ := range ld.SortOrder {
		ml, err := ld.Alphabet().Val(runeLetter)
		if err != nil {
			panic(err)
		}
		mlAsIndex := uint32(ml)
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

	// Write the distribution
	binary.Write(file, binary.BigEndian, dist)

	// Write the point values
	binary.Write(file, binary.BigEndian, pointValues)

	// Write the vowels boolean array
	binary.Write(file, binary.BigEndian, isVowel)

	file.Close()
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
		userVisibleLetters[i] = uint32(ml.UserVisible(alph))
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
		ml, err := alph.Val(rune(i))
		if err != nil {
			machineLetters[i] = uint32(invalidMachineLetterValue)
		} else {
			machineLetters[i] = uint32(ml)
		}
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
	createLaddag(lexicon, gaddag.Alphabet)
	createLetterDistribution(letterDistribution)
	createLetterConversion(lexicon, gaddag.Alphabet)
}
