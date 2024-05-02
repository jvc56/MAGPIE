#ifndef TEST_CONSTANTS_H
#define TEST_CONSTANTS_H

#define TESTDATA_FILEPATH "test/testdata/"

#define EMPTY_CGP                                                              \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 lex CSW21;"
#define EMPTY_CATALAN_CGP                                                      \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 lex DISC2;"
#define EMPTY_POLISH_CGP                                                       \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 lex OSPS49;"
#define EMPTY_PLAYER0_RACK_CGP                                                 \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 /ABC 0/0 0 lex CSW21;"
#define EMPTY_PLAYER1_RACK_CGP                                                 \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/ 0/0 0 lex CSW21;"
#define OPENING_CGP                                                            \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? 0/0 0 lex "    \
  "CSW21;"
#define ONE_CONSECUTIVE_ZERO_CGP                                               \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? 0/0 1 lex "    \
  "CSW21;"
#define EXCESSIVE_WHITESPACE_CGP                                               \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15       ABCDEFG/HIJKLM?        " \
  "  0/0     4      lex     CSW21;"
#define DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP                                      \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DINNVWY/DEIMNRT 0/0 0 lex "    \
  "CSW21;"
#define DOUG_V_EMELY_CGP                                                       \
  "15/15/15/15/15/15/15/3WINDY7/15/15/15/15/15/15/15 ADEEGIL/AEILOUY 0/32 0 "  \
  "lex CSW21;"
#define GUY_VS_BOT_ALMOST_COMPLETE_CGP                                         \
  "15/15/15/15/15/15/15/7AGAVE3/15/15/15/15/15/15/15 AEFPRU?/ABEIJNO 20/0 1 "  \
  "lex CSW21;"
#define GUY_VS_BOT_CGP                                                         \
  "15/15/15/15/15/15/1FlAREUP7/7AGAVE3/15/15/15/15/15/15/15 AEINNO?/AHIOSTU "  \
  "0/86 0 lex CSW21;"
#define INCOMPLETE_3_CGP                                                       \
  "15/15/15/15/15/15/15/6GUM6/7PEW5/9EF4/9BEVEL1/15/15/15/15 AENNRSV/BDIIPSU " \
  "26/56 0 lex CSW21;"
#define INCOMPLETE4_CGP                                                        \
  "15/15/15/15/14V/14A/14N/6GUM5N/7PEW4E/9EF3R/9BEVELS/15/15/15/15 "           \
  "AEEIILZ/CDGKNOR 56/117 0 lex CSW21;"
#define INCOMPLETE_ELISE_CGP                                                   \
  "15/15/15/15/14V/9MALTHA/14N/6GUM5N/7PEW4E/9EF3R/9BEVELS/15/15/15/15 "       \
  "BDDDOST/CIKORRS 117/81 0 lex CSW21;"
#define INCOMPLETE_CGP                                                         \
  "15/15/15/15/14V/9MALTHA/14N/6GUM3O1N/7PEW2D1E/9EF1D1R/9BEVELS/12S2/12T2/"   \
  "15/15 EGOOY/ACEIO?? 81/137 0 lex CSW21;"
#define JOSH2_CGP                                                              \
  "15/15/10T4/10AD3/10MO3/5ZEK2EW3/6MITT1N3/7DOWLY3/8POI4/3ALBUGoS5/15/15/15/" \
  "15/15 CGILLRS/EFNNOOU 98/167 0 lex CSW21;"
#define NAME_ISO8859_1_CGP                                                     \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DINNVWY/ALLPTVZ 0/0 0 lex "    \
  "CSW21;"
#define NAME_UTF8_NOHEADER_CGP                                                 \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DINNVWY/AJLORUV 0/0 0 lex "    \
  "CSW21;"
#define NAME_UTF8_WITH_HEADER_CGP                                              \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DINNVWY/AEILMRU 0/0 0 lex "    \
  "CSW21;"
#define NOAH_VS_MISHU_CGP                                                      \
  "15/15/15/15/15/15/12BOA/6VOX1ATONY/7FIVER3/11E3/8MOANED1/7QI2C3/8MU1H3/15/" \
  "15 AAIRSSU/AELORTT 76/120 0 lex CSW21;"
#define NOAH_VS_PETER_CGP                                                      \
  "15/9O5/7GIP5/7H1E3T1/7E5E1/5CUTTY3EF/7T6O/7OR4UP/7SEMINAL1/4AX2S4V1/"       \
  "2DILUTION3AE/8L5V/8D5I/8e5T/8R5E AFK/ABIMQSW 172/249 0 lex CSW21;"
#define SOME_ISC_GAME_CGP                                                      \
  "15/1V13/1O13/1D12P/1U12O/1NA2C7FE/2N2R7AM/OUTFLYING2AHIS/2I1O2AILERON1/"    \
  "2R1O10/2I1I2D2WAG2/2O1EXPUNgES3/2t4C7/6SKOALING1/7Y7 EEHMRTZ/AEIJQRU "      \
  "264/306 0 lex CSW21;"
#define UTF8_DOS_CGP                                                           \
  "15/15/15/5QI8/2ERMINE7/7WAZ5/4C3POLL2E/3BLONDE1AIDOI/4A2E6N/4T2NAVY3S/"     \
  "2F1C1LA2OU2T/2REH1OR6E/2YU2XI6I/3K3U6N/7S3JIGS BDHIRT?/ADERTUW 279/284 0 "  \
  "lex CSW21;"
#define VS_ANDY_CGP                                                            \
  "12T2/11JOB1/11ORE1/B10LIDO/AEGISES1MAUT2R/G5THEW4I/N4ZA2LIFT1g/I5GYP5I/"    \
  "O13N/7DRUtHERS/7A1MOOL2/7C3EF2/7I7/7T7/7E7 UV/DENQRUW 277/240 0 lex CSW21;"
#define VS_FRENTZ_CGP                                                          \
  "15/4EN9/4NONVIRILE2/3ADO9/3WOK9/3AW10/4E10/3CRAAlED5/4S1XI7/5YEP1GOR3/"     \
  "2JIBE1EUOI4/1SAFE10/2IF2ACErBER2/GUL1TUM8/2STERILE6 ADDIPYZ/HHLRTTU "       \
  "320/373 0 lex CSW21;"

#define VS_ED                                                                  \
  "14E/14N/14d/14U/4GLOWS5R/8PET3E/7FAXING1R/6JAY1TEEMS/2B2BOY4N2/2L1DOE5U2/"  \
  "2ANEW5PI2/2MO1LEU3ON2/2EH7HE2/15/15 / 0/0 0 lex NWL20;"
#define VS_JEREMY                                                              \
  "7N6M/5ZOON4AA/7B5UN/2S4L3LADY/2T4E2QI1I1/2A2PORN3NOR/2BICE2AA1DA1E/"        \
  "6GUVS1OP1F/8ET1LA1U/5J3R1E1UT/4VOTE1I1R1NE/5G1MICKIES1/6FE1T1THEW/"         \
  "6OR3E1XI/6OY6G / 0/0 0 lex NWL20;"
#define VS_JEREMY_WITH_P2_RACK                                                 \
  "7N6M/5ZOON4AA/7B5UN/2S4L3LADY/2T4E2QI1I1/2A2PORN3NOR/2BICE2AA1DA1E/"        \
  "6GUVS1OP1F/8ET1LA1U/5J3R1E1UT/4VOTE1I1R1NE/5G1MICKIES1/6FE1T1THEW/"         \
  "6OR3E1XI/6OY6G /AHIILR 0/0 0 lex NWL20;"
#define VS_MATT                                                                \
  "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/6JABS1b3/" \
  "5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 / 0/0 0 lex " \
  "NWL20;"
#define VS_MATT2                                                               \
  "14R/12Q1E/2TIGER4HI1I/6OF3U2N/4OCEAN1PRANK/7BAZAR3/11A3/7MOONY3/6DIF6/"     \
  "5VEG7/7SAnTOOR1/10OX3/7AGUE4/15/15 / 0/0 0 lex NWL20;"
#define VS_OXY                                                                 \
  "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"      \
  "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 / 0/0 0 lex "  \
  "NWL20;"
#define TEST_DUPE                                                              \
  "15/15/15/15/15/15/15/1INCITES7/IS13/T14/15/15/15/15/15 / 0/0 0 lex "        \
  "NWL20;"

#define MANY_MOVES                                                             \
  "7P7/7A7/7R7/7T7/7E7/7R7/4P2RETRACED/1ORDINEE3S3/4C6T3/4T6O3/4U6N3/4R6I3/"   \
  "4A6E3/4L6S3/15 AEINS??/DEIKLYY 139/221 0 lex CSW21;"
#define KA_OPENING_CGP                                                         \
  "15/15/15/15/15/15/15/6KA7/15/15/15/15/15/15/15 ADEEGIL/AEILOUY 0/4 0 lex "  \
  "CSW21;"
#define AA_OPENING_CGP                                                         \
  "15/15/15/15/15/15/15/6AA7/15/15/15/15/15/15/15 ADEEGIL/AEILOUY 0/4 0 lex "  \
  "CSW21;"
#define QI_WITH_Q_ON_STAR_OPENING_CGP                                          \
  "15/15/15/15/15/15/15/7QI6/15/15/15/15/15/15/15 / 0/22 0 lex CSW21;"
#define TRIPLE_LETTERS_CGP                                                     \
  "15/15/15/15/15/15/1PROTEAN7/3WINDY7/15/15/15/15/15/15/15 ADEEGIL/AEILOUY "  \
  "0/32 0 lex CSW21;"
#define TRIPLE_DOUBLE_CGP                                                      \
  "15/15/15/15/15/15/15/15/15/2PAV10/15/15/15/15/15 ADEEGIL/AEILOUY 0/32 0 "   \
  "lex CSW21;"
#define BOTTOM_LEFT_RE_CGP                                                     \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/RE13 ADEEGIL/AEILOUY 0/32 0 lex " \
  "CSW21;"
#define LATER_BETWEEN_DOUBLE_WORDS_CGP                                         \
  "15/15/15/15/5LATER5/15/15/15/15/15/15/15/15/15/15 ADEEGIL/AEILOUY 0/32 0 "  \
  "lex CSW21;"
#define ION_OPENING_CGP                                                        \
  "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 ADEEGIL/AEILOUY 0/4 0 lex " \
  "CSW21;"
#define ZILLION_OPENING_CGP                                                    \
  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 IILLNOZ/ 0/4 0 lex "           \
  "CSW21;"
#define ENTASIS_OPENING_CGP                                                    \
  "15/15/15/15/15/15/15/2ENTASIS6/15/15/15/15/15/15/15 / 0/0 0 lex CSW21"
#define UEY_CGP                                                                \
  "T2F3C7/O2O1BEHOWLING1/A1PI2ME4IO1/s1OD1NUR2AIDA1/T1L1NARTJIES3/I1E2NE1I6/"  \
  "E1A2N2B6/SAXONY1UEY5/2E5D6/15/15/15/15/15/15 ACEOOQV/?DEGPRS 271/283 0 "    \
  "lex CSW21;"
#define OOPSYCHOLOGY_CGP                                                       \
  "1OOPSYCHOLOGY2/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 lex "      \
  "CSW21;"
#define DELDAR_VS_HARSHAN_CGP                                                  \
  "15/15/10LAZED/6EPITAXES1/7R7/7o7/7T7/7E7/7G7/7E7/2SNIFTER6/15/15/15/15 "    \
  "IJLNOPS/ 149/154 0 lex CSW21;"
#define CATALAN_CGP                                                            \
  "15/15/15/11HUNS/4E5JO3/4n2CE[L·L]A4/2RECELAT6/3XAMIS2D4/4R1BO[NY]1E1P1E/"  \
  "1ZELAM4S1E1N/4R5A1V1C/BUS[QU]IN4FRENA/U3A2MODI1T1U/R6ERINOSIS/"             \
  "LITIGAnT2O3E AAIPRRS/AADEGLT 388/446 0 lex DISC2;"
#define POLISH_CGP                                                                 \
  "15/15/11FiŚ1/11LI2/9CŁA3/9Z1N3/8HOI4/6STĘPIĆ3/5AUR1Y5/4SAMY2G4/2CLE4JA1K2/" \
  "2LARWO1SAMBIE1/2I1WENTO1O1IW1/E6END3Y1/ZDZIAŁaJ1Y5 ACNNPRW/AHIKPZŻ "          \
  "240/200 0 lex OSPS49;"
#define FRAWZEY_CGP                                                            \
  "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/7K7/7E7 /  150/224 " \
  "0 lex CSW21;"
#define THERMOS_CGP                                                            \
  "15/15/3THERMOS2A2/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 lex CSW21;"

// 21 x 21 boards

#define ANTHROPOMORPHISATIONS_CGP                                              \
  "21/21/21/21/21/21/21/21/21/21/6POMORPHISATION1/21/21/21/21/21/21/21/21/21/" \
  "21 / 0/0 0 "                                                                \
  "lex CSW21;"

#endif