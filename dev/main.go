package main

import (
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
	RunComparisonTests()
}

func main() {
	zerolog.SetGlobalLevel(zerolog.Disabled)
	NondeterministicTests()
}
