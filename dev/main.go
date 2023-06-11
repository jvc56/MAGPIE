package main

import (
	"flag"
	"fmt"
	"sync"

	"github.com/rs/zerolog"
)

func testDev() {
	// racks := []string{
	// 	"UUUVVWW",
	// 	"AEFRWYZ",
	// 	"INOOQSU",
	// 	"LUUUVVW",
	// 	"EEEEEOO",
	// 	"AEIKLMO",
	// 	"GNOOOPR",
	// 	"EGIJLRS",
	// 	"EEEOTTT",
	// 	"EIILRSX",
	// 	"?CEEILT",
	// 	"?AFERST",
	// 	"AAAAAAI",
	// 	"GUUUVVW",
	// 	"AEEEEEO",
	// 	"AAAAAII",
	// 	"AEUUUVV",
	// 	"AEEEEII",
	// 	"AACDENU",
	// }
	// PrintCGPTurns(racks)

	// racks := []string{
	// 	"AEGILPR",
	// 	"ACELNTV",
	// 	"DDEIOTY",
	// 	"?ADIIUU",
	// 	"?BEIINS",
	// 	"EEEKMNO",
	// 	"AAEHINT",
	// 	"CDEGORZ",
	// 	"EGNOQRS",
	// 	"AFIQRRT",
	// 	"ERSSTTX",
	// 	"BGHNOOU",
	// 	"AENRTUZ",
	// 	"AFIMNRV",
	// 	"AEELNOT",
	// 	"?EORTUW",
	// 	"ILNOOST",
	// 	"EEINRUY",
	// 	"?AENRTU",
	// 	"EEINRUW",
	// 	"AJNPRV",
	// 	"INRU",
	// 	"PRV",
	// 	"U",
	// 	"RV",
	// 	"U",
	// 	"V",
	// 	"U",
	// 	"V",
	// 	"U",
	// 	"V",
	// }

	// PrintCGPTurns(racks)

	// mg := FindRandomSixPass()
	// fmt.Println(mg.ToDisplayText())
	// fmt.Println(gcgio.GameHistoryToGCG(mg.History(), false))

	racks := []string{
		"EGIILNO",
		"DRRTYYZ",
		"CEIOTTU",
		"AADEEMT",
		"AACDEKS",
		"BEEIOOP",
		"DHLNORR",
		"BGIIJRV",
		"?DFMNPU",
		"EEEOQRW",
		"IINNSVW",
		"?ADEOPU",
		"EFOTTUV",
		"ADHIMNX",
		"CEFINQS",
		"?ADRSTT",
		"?CIRRSU",
		"AEEFGIL",
		"EEGHLMN",
		"AAAEELL",
		"DEEGLNN",
		"AEGILUY",
		"EN",
	}

	PrintCGPTurns(racks)

	// mg := FindRandomStandard()
	// fmt.Println(mg.ToDisplayText())
	// fmt.Println(gcgio.GameHistoryToGCG(mg.History(), false))
}

func NondeterministicTests() {
	zerolog.SetGlobalLevel(zerolog.Disabled)
	threads := 12
	var wg sync.WaitGroup
	wg.Add(threads)
	for i := 0; i < threads; i++ {
		go RunComparisonTests(fmt.Sprintf("Thread %d", i))
	}
	wg.Wait()
}

func main() {
	// Define flags
	lexicon := flag.String("lexicon", "", "the lexicon argument")
	gcg := flag.String("gcg", "", "the gcg argument")
	turnNumber := flag.Int("turn_number", 0, "the turn_number argument")
	margin := flag.Float64("margin", 0.0, "the margin argument")
	partial := flag.Bool("partial", false, "use racks in the GCG to infer partial leaves")

	// Parse command line arguments
	flag.Parse()

	// Check if all required flags are provided
	if *lexicon == "" || *gcg == "" || *turnNumber == 0 {
		flag.PrintDefaults()
		return
	}

	Infer(*lexicon, *gcg, *turnNumber, *margin, *partial)
}
