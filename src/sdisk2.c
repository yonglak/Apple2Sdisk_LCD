/*------------------------------------

	SDISK II LCD Firmware (1 of 2)
	
	2010.11.11 by Koichi Nishida
	2012.01.26 by F�bio Belavenuto

------------------------------------*/

/*
 * Added support for image exchange using a button added in the Brazilian version by Victor Trucco
 * Added support for a 16x2 LCD
 */

/*
hardware information:

use ATMEGA328P AVR.
connect 27MHz (overclock...) crystal to the AVR.
3.3V power.

fuse setting :
	LOW: 11011110
	HIGH: default

connection:
pin		signal	description
2		D0		DO (SD card)
3		D1		CS (SD card)
4 		D2		WRITE REQUEST (APPLE II disk IF, pull up with 10K ohm)
5		D3		EJECT switch (LOW if SD card is inserted)
6		D4		DI (SD card)                                           and LCD D6
11		D5		CLK (SD card)                                          and LCD D7
12		D6		ENTER switch (LOW when pushed)
13		D7		DOWN switch (LOW when pushed)			-
14		B0		PHASE-0 (APPLE II disk IF)
15		B1		PHASE-1 (APPLE II disk IF)
16		B2		PHASE-2 (APPLE II disk IF)
17		B3		PHASE-3 (APPLE II disk IF)
18		B4		red LED (through 330 ohm, on when HIGH)
19		B5		UP switch (LOW when pushed)      		-
9-10	B6-B7	connect to the crystal
23		C0		DRIVE ENABLE (APPLE II disk IF)
24		C1		READ PULSE (APPLE II disk IF through 74HC125 3state)    and LCD D4
25		C2		WRITE (APPLE II disk IF)
26		C3		WRITE PROTECT (APPLE II disk IF through 74HC125 3state) and LCD D5
27		C4		LCD RS
28		C5		LCD E
1		C6		/RESET

	
	Note that the enable input of the 3state buffer 74HC125,
	should be connected with DRIVE ENABLE.
*/

/*
This is a part of the firmware for DISK II emulator by Nishida Radio.

Copyright (C) 2010 Koichi NISHIDA
email to Koichi NISHIDA: tulip-house@msf.biglobe.ne.jp

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include "string.h"
#include "config.h"

#define WAIT 1
#define BUF_NUM 5
#define FAT_DSK_ELEMS 18
#define FAT_NIC_ELEMS 35
#define nop() __asm__ __volatile__ ("nop")

// C prototypes

// cancel read
void cancelRead(void);
// write a byte data to the SD card
void writeByteSlow(unsigned char c);
void writeByteFast(unsigned char c);
// read data from the SD card
unsigned char readByteSlow(void);
unsigned char readByteFast(void);
// wait until finish a command
void waitFinish(void);
// issue SD card command slowly without getting response
void cmd_(unsigned char cmd, unsigned long adr);
// issue SD card command fast and wait normal response
void cmdFast(unsigned char cmd, unsigned long adr);
// get command response slowly from the SD card
unsigned char getRespSlow(void);
// get command response fast from the SD card
unsigned char getRespFast(void);
// issue command 17 and get ready for reading
void cmd17Fast(unsigned long adr);
// display a string to LCD
void dispStr(char *str, unsigned char f);
// get a file name from a directory entry
void getFileName(unsigned short dir, char *name);
// find a file whose extension is targExt,
// and whose name is targName if withName is true
int findExt(char *targExt, unsigned char *protect,
	char *targName, unsigned char withName);
// prepare the FAT table on memory
void prepareFat(int i, unsigned short *fat, unsigned short len,
	unsigned char fatNum, unsigned char fatElemNum);
// memory copy	
void memcp(unsigned char *dst, unsigned char *src, const unsigned short len);
// duplicate FAT for FAT16
void duplicateFat(void);
// write to the SD cart one by one
void writeSD(unsigned long adr, unsigned char *data, unsigned short len);
// create a NIC image file
int createFile(char *name, char *ext, unsigned short sectNum);
// translate a DSK image into a NIC image
void dsk2Nic(void);
// make file name list
unsigned short makeFileNameList(unsigned short *list, char *targExt);
// choose a NIC file from a NIC file name list
unsigned char chooseANicFile(void *tempBuff, unsigned char btfExists, char *filebase);
// initialization called from check_eject
void init(unsigned char choose);
// called when the SD card is inserted or removed
void check_eject(void);
// buffer clear
void buffClear(void);
// Low-level LCD transfer 4 bits
void lcd_port(unsigned char c);
// LCD instruction
void lcd_cmd(unsigned char c);
// LCD data
void lcd_data(unsigned char c);
// Send a LCD ASCII char
void lcd_char(unsigned char c);
// Init LCD
void lcd_init(void);
// clear LCD
void lcd_clear(void);
// output a character on LCD
void lcd_char(unsigned char c);
// Clear LCD
void lcd_clear(void);
// Goto X,Y
void lcd_gotoxy(unsigned char x, unsigned char y);
// output a string on LCD
void lcd_puts(const char *str);
// output a PROGMEM string on LCD
void lcd_puts_p(prog_char *progmem_s);

// assembler functions
// see sub.S file
void wait5(unsigned short time);

// write data back to a NIC image
void writeBack(void);
void writeBackSub(void);
void writeBackSub2(unsigned char bn, unsigned char sc, unsigned char track);

// SD card information
unsigned long bpbAddr, rootAddr;
unsigned long fatAddr;					// the beginning of FAT
//unsigned short fileFatTop;
unsigned char sectorsPerCluster, sectorsPerCluster2;	// sectors per cluster
unsigned short sectorsPerFat;	
unsigned long userAddr;					// the beginning of user data
// unsigned short fatDsk[FAT_DSK_ELEMS];// use writeData instead
unsigned short fatNic[FAT_NIC_ELEMS];
unsigned char prevFatNumDsk, prevFatNumNic;
unsigned short nicDir, dskDir, btfDir;

// DISK II status
unsigned char ph_track;					// 0 - 139
unsigned char sector;					// 0 - 15
unsigned short bitbyte;					// 0 - (8*512-1)
unsigned char prepare;
unsigned char readPulse;
unsigned char inited;
unsigned char magState;
unsigned char protect;
unsigned char formatting;
const unsigned char volume = 0xfe;

// write data buffer
unsigned char writeData[BUF_NUM][350];
unsigned char sectors[BUF_NUM], tracks[BUF_NUM];
unsigned char buffNum;
unsigned char *writePtr;

// a table for head stepper moter movement 
PROGMEM prog_uchar stepper_table[4] = {0x0f,0xed,0x03,0x21};

// encode / decode table for a nib image
PROGMEM prog_uchar encTable[] = {
	0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,
	0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
	0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,
	0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
	0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,
	0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
	0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,
	0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

// a table for translating logical sectors into physical sectors
PROGMEM prog_uchar physicalSector[] = {
		0,13,11,9,7,5,3,1,14,12,10,8,6,4,2,15};

// for bit flip
PROGMEM prog_uchar FlipBit[] = { 0,  2,  1,  3  };
PROGMEM prog_uchar FlipBit1[] = { 0, 2,  1,  3  };
PROGMEM prog_uchar FlipBit2[] = { 0, 8,  4,  12 };
PROGMEM prog_uchar FlipBit3[] = { 0, 32, 16, 48 };

/* Mensagens */
/*                     1234567890123456 */
PROGMEM char MSG1[] = "   SDISK2 LCD   ";
PROGMEM char MSG2[] = " by Apple II BR ";
PROGMEM char MSG3[] = "Mounted ";
PROGMEM char MSG4[] = " Disk : ";
PROGMEM char MSG5[] = "Load   Directory";
PROGMEM char MSG6[] = "Select a Disk : ";
PROGMEM char MSG7[] = "Loading ........";
PROGMEM char MSG8[] = "   No SD Card   ";


/* Defini��es para o LCD */
#define LCD_ENABLE  				PORTC |= _BV(5)
#define LCD_DISABLE 				PORTC &=~_BV(5)
#define LCD_INSTRUCTION				PORTC &=~_BV(4)
#define LCD_DATA					PORTC |= _BV(4)

/* Rotinas para o LCD */
// ------------------------------------
void lcd_port(unsigned char c)
{
	LCD_DISABLE;
	if (c & 0x01) PORTC |= _BV(1); else PORTC &=~_BV(1);
	if (c & 0x02) PORTC |= _BV(3); else PORTC &=~_BV(3);
	if (c & 0x04) PORTD |= _BV(4); else PORTD &=~_BV(4);
	if (c & 0x08) PORTD |= _BV(5); else PORTD &=~_BV(5);
	LCD_ENABLE;
	_delay_us(1);
	LCD_DISABLE;
	_delay_us(1);
}

// ------------------------------------
void lcd_cmd(unsigned char c)
{
	PORTD |= _BV(1);	// SD CS=1 - SD Desabilitado
	LCD_INSTRUCTION;
	lcd_port(c >> 4);
	lcd_port(c & 0x0F);
	if (c & 0xFC)
		_delay_us(60);		// espera 60uS para outros comandos
	else
		_delay_us(1700);	// espera 1,7ms para comandos 0x01 e 0x02
}

// ------------------------------------
void lcd_data(unsigned char c)
{
	PORTD |= _BV(1);	// SD CS=1 - SD Desabilitado
	LCD_DATA;
	lcd_port(c >> 4);
	lcd_port(c & 0x0F);
	_delay_us(100);			// 100uS para dados
}

// LCD init
void lcd_init()
{
	LCD_INSTRUCTION;
	_delay_ms(50);
	lcd_port(0x03);
	_delay_ms(5);
	lcd_port(0x03);
	_delay_ms(5);
	lcd_port(0x03);
	_delay_ms(5);
	lcd_port(0x02);
	_delay_ms(5);
	lcd_cmd(0x28);		// 4 bits, 2 linhas
	lcd_cmd(0x08);		// display off
	lcd_cmd(0x01);		// clear
	lcd_cmd(0x02);		// home
	lcd_cmd(0x06);		// Cursor incrementa para direita
	lcd_cmd(0x0C);		// Display on, cursor off
}

// clear lcd
void lcd_clear()
{
	lcd_cmd(0x01);		// clear
	lcd_cmd(0x02);		// home
}

// Goto X,Y
void lcd_gotoxy(unsigned char x, unsigned char y) {
	x &= 0x0F;
	y &= 0x01;
	if (y == 0)
		lcd_cmd(0x80 + x);
	else
		lcd_cmd(0xC0 + x);
}

// output a character on LCD
void lcd_char(unsigned char c)
{
	//if (c == '~') c = ' ';
	lcd_data(c);
}

/******************************************************************************/
void lcd_puts(const char *str) {
	register char c;

	while( (c = *(str++)))
		lcd_char(c);
}

/******************************************************************************/
void lcd_puts_p(prog_char *progmem_s) {
    register char c;

    while ( (c = pgm_read_byte(progmem_s++)) ) {
    	lcd_char(c);
    }

}
/******************************************************************************/
// display a 8-byte string to LCD
void dispStr(char *str, unsigned char f)
{
	unsigned char i;
	if (f==0) {
		lcd_clear();
		lcd_gotoxy(0, 1);	// Linha 2, coluna 1
		lcd_puts_p(MSG3);
	} else {
		lcd_gotoxy(0, 1);	// Linha 2, coluna 1
		lcd_puts_p(MSG4);
	}
	for (i = 0; i != 8; i++)
		lcd_char(*(str++));
}

/**************************************************************************/

// buffer clear
void buffClear(void)
{
	unsigned char i;
	unsigned short j;
	
	for (i=0; i<BUF_NUM; i++)
		for (j=0; j<350; j++)
			writeData[i][j]=0;
	for (i=0; i<BUF_NUM; i++)
		sectors[i]=tracks[i]=0xff;
}

/******************************************************************************/
// cancel read from the SD card
void cancelRead(void)
{
	unsigned short i;
	if (bitbyte < (402 * 8)) {
		PORTD = NCLK_DINCS;
		for (i = bitbyte; i < (514 * 8); i++) {		// 512 bytes + 2 CRC
			if (bit_is_set(PIND, 3)) return;
			PORTD = _CLK_DINCS;
			PORTD = NCLK_DINCS;
		}
		bitbyte = 402 * 8;
	}
}

/******************************************************************************/
// write a byte data to the SD card
void writeByteSlow(unsigned char c)
{
	unsigned char d;
	for (d = 0b10000000; d; d >>= 1) {
		if (c & d) {
			PORTD = NCLK_DINCS;
			wait5(WAIT);
			PORTD = _CLK_DINCS;
		} else {
			PORTD = NCLKNDINCS;
			wait5(WAIT);
			PORTD = _CLKNDINCS;
		}
		wait5(WAIT);
	}

	PORTD = NCLKNDINCS;
}

/******************************************************************************/
void writeByteFast(unsigned char c)
{
	unsigned char d;
	for (d = 0b10000000; d; d >>= 1) {
		if (c & d) {
			PORTD = NCLK_DINCS;
			PORTD = _CLK_DINCS;
		} else {
			PORTD = NCLKNDINCS;
			PORTD = _CLKNDINCS;
		}
	}
	PORTD = NCLKNDINCS;
}

/******************************************************************************/
// read data from the SD card
unsigned char readByteSlow(void)
{
	unsigned char c = 0;
	volatile unsigned char i;

	PORTD = NCLK_DINCS;
	wait5(WAIT);
	for (i = 0; i != 8; i++) {
		PORTD = _CLK_DINCS;
		wait5(WAIT);
		c = ((c << 1) | (PIND & 1));
		PORTD = NCLK_DINCS;
		wait5(WAIT);	
	}
	return c;
}

/******************************************************************************/
unsigned char readByteFast(void)
{
	unsigned char c = 0;
	volatile unsigned char i;

	PORTD = NCLK_DINCS;
	for (i = 0; i != 8; i++) {
		PORTD = _CLK_DINCS;
		c = ((c << 1) | (PIND & 1));
		PORTD = NCLK_DINCS;
	}
	return c;
}

/******************************************************************************/
// wait until data is written to the SD card
void waitFinish(void)
{
	unsigned char ch;
	do {
		ch = readByteFast();
		if (bit_is_set(PIND, 3)) return;
	} while (ch != 0xff);
}

/******************************************************************************/
// issue a SD card command slowly without getting response
void cmd_(unsigned char cmd, unsigned long adr)
{
	writeByteSlow(0xff);
	writeByteSlow(0x40 + cmd);
	writeByteSlow(adr >> 24);
	writeByteSlow((adr >> 16) & 0xff);
	writeByteSlow((adr >> 8) & 0xff);
	writeByteSlow(adr & 0xff);
	writeByteSlow(0x95);
	writeByteSlow(0xff);
}

/******************************************************************************/
// issue a SD card command and wait normal response
void cmdFast(unsigned char cmd, unsigned long adr)
{
	unsigned char res;
	do {
		writeByteFast(0xff);
		writeByteFast(0x40 + cmd);
		writeByteFast(adr >> 24);
		writeByteFast((adr >> 16) & 0xff);
		writeByteFast((adr >> 8) & 0xff);
		writeByteFast(adr & 0xff);
		writeByteFast(0x95);
		writeByteFast(0xff);
	} while (((res=getRespFast()) != 0) && (res != 0xff));
}

/******************************************************************************/
// get a command response slowly from the SD card
unsigned char getRespSlow(void)
{
	unsigned char ch;
	do {
		ch = readByteSlow();
		if (bit_is_set(PIND, 3)) return 0xff;
	} while ((ch & 0x80) != 0);
	return ch;
}

/******************************************************************************/
// get a command response fast from the SD card
unsigned char getRespFast(void)
{
	unsigned char ch;
	do {
		ch = readByteFast();
		if (bit_is_set(PIND, 3)) return 0xff;
	} while ((ch & 0x80) != 0);
	return ch;
}

/******************************************************************************/
// issue command 17 and get ready for reading
void cmd17Fast(unsigned long adr)
{
	unsigned char ch;

	cmdFast(17, adr);
	do {	
		ch = readByteFast();
		if (bit_is_set(PIND, 3)) return;
	} while (ch != 0xfe);
}

/******************************************************************************/
// get a file name from a directory entry
void getFileName(unsigned short dir, char *name)
{
	unsigned char i;

	cmdFast(16, 8);
	cmd17Fast(rootAddr + dir * 32);
	for (i = 0; i != 8; i++) *(name++) = (char)readByteFast();
	readByteFast(); readByteFast(); // discard CRC bytes
}

/******************************************************************************/
// find a file whose extension is targExt,
// and whose name is targName if withName is true.
int findExt(char *targExt, unsigned char *protect, char *targName, unsigned char withName)
{
	short i, j;
	unsigned max_file = 512;
	unsigned short max_time = 0, max_date = 0;

	// find NIC extension
	for (i=0; i != 512; i++) {
		unsigned char name[8], ext[3], d;
		unsigned char time[2], date[2];

		if (bit_is_set(PIND, 3)) return 512;
		// check first char
		cmdFast(16, 1);
		cmd17Fast(rootAddr + i * 32);
		d = readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes
		if ((d == 0x00) || (d == 0x05) || (d == 0x2e) || (d == 0xe5)) continue;		// Exclu�do
		if (!(((d >= 'A') && (d <= 'Z')) || ((d >= '0') && (d <= '9')))) continue;	// Inv�lido
		cmd17Fast(rootAddr+i*32+11);												// Atributos
		d = readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes
		if (d & 0x1e) continue;														// Escondido, de sistema, volume ou diret�rio
		if (d == 0xf) continue;														// Registro LFN
		// check extension
		cmdFast(16, 12);
		cmd17Fast(rootAddr + i * 32);
		for (j=0; j!=8; j++) name[j] = readByteFast();								// Nome
		for (j=0; j!=3; j++) ext[j] = readByteFast();								// Extens�o
		if (protect)
			*protect = ((readByteFast() & 1) << 3);									// Somente leitura
		else
			readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes

		// check time stamp
		cmdFast(16, 4);
		cmd17Fast(rootAddr + i * 32 + 22);
		time[0] = readByteFast();
		time[1] = readByteFast();
		date[0] = readByteFast();
		date[1] = readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes

		if (memcmp(ext, targExt, 3) == 0) {											// Extens�o achada
			if ((!withName) || (targName && memcmp(name, targName, 8) == 0)) {		// Se n�o estiver procurando pelo nome ou foi passado um ponteiro
				unsigned short tm = *(unsigned short *)time;						// de nome de arquivo e esse corresponder ao achado
				unsigned short dt = *(unsigned short *)date;

				if ((dt > max_date) || ((dt == max_date) && (tm >= max_time))) {	// Marca arquivo com data maior
					max_time = tm;
					max_date = dt;
					max_file = i;
				}
			}
		}
	}

	if ((max_file != 512) && (targName != 0) && (!withName)) {						// Se achou arquivo e foi passado um ponteiro para o nome do arquivo
		unsigned char j;															// e n�o est� procurando por um nome, copia o nome achado para o ponteiro
		cmdFast(16, 8);
		cmd17Fast(rootAddr + max_file * 32);
		for (j = 0; j < 8; j++) targName[j] = readByteFast();
		readByteFast(); readByteFast();
	}
	return max_file;
	// if 512 then not found...
}

/******************************************************************************/
// prepare a FAT table on memory
// L� a cadeia de clusters do arquivo #(i) de (len) clusters, limitando � (fatElemNum) clusters
void prepareFat(int i, unsigned short *fat, unsigned short len, unsigned char fatNum, unsigned char fatElemNum)
{
	unsigned short ft;
	unsigned char fn;

	if (bit_is_set(PIND, 3)) return;												// Cart�o foi removido
	cmdFast(16, (unsigned long)2);
	cmd17Fast(rootAddr + i * 32 + 26);												// Ler cluster inicial desse arquivo
	ft = readByteFast();
	ft += (unsigned short)readByteFast() * 0x100;									// Cluster � 16 bits little-endian
	readByteFast(); readByteFast(); // discard CRC bytes
	if (0 == fatNum) fat[0] = ft;													// ?
	for (i = 0; i < len; i++) {
		fn = (i + 1) / fatElemNum;
		cmd17Fast((unsigned long)fatAddr + (unsigned long)ft * 2);					// L� pr�ximo cluster
		ft = readByteFast();
		ft += (unsigned short)readByteFast()*0x100;
		readByteFast(); readByteFast(); // discard CRC bytes
		if (fn == fatNum) fat[(i + 1) % fatElemNum] = ft;							// Salva # cluster na lista
		if ((ft > 0xfff6) || (fn > fatNum))											// Se cluster for inv�lido ou final, ou extrapolar
			break;																	// limite da tabela
	}
	cmdFast(16, (unsigned long)512);												// Prepara para ler 512 bytes
}

/******************************************************************************/
// memory copy
void memcp(unsigned char *dst, unsigned char *src, unsigned short len)
{
	unsigned short i;
	
	for (i = 0; i < len; i++) dst[i] = src[i];
}

/******************************************************************************/
// write to the SD cart one by one
void writeSD(unsigned long adr, unsigned char *data, unsigned short len)
{
	unsigned int i;
	unsigned char *buf = &writeData[0][0];

	if (bit_is_set(PIND, 3)) return;												// Cart�o foi removido

	cmdFast(16, 512);																// Ler 512 bytes
	cmd17Fast(adr & 0xfffffe00);													// Filtrar endere�o
	for (i = 0; i < 512; i++) buf[i] = readByteFast();								// Ler e salvar em *buf
	readByteFast(); readByteFast(); // discard CRC bytes
	memcp( &(buf[adr & 0x1ff]), data, len);											// Copiar dados para *buf
	
	PORTD = NCLKNDI_CS;
	PORTD = NCLKNDINCS;
				
	cmdFast(24, adr & 0xfffffe00);													// Endere�o de grava��o
	writeByteFast(0xff);															// Obrigat�rio enviar isso
	writeByteFast(0xfe);															// Obrigat�rio enviar isso
	for (i = 0; i < 512; i++) writeByteFast(buf[i]);								// Enviar dados para grava��o
	writeByteFast(0xff);															// CRC falso
	writeByteFast(0xff);															// CRC falso
	readByteFast();																	// Ler byte de status (ignora)
	waitFinish();																	// Espera terminar a grava��o
	
	PORTD = NCLKNDI_CS;
	PORTD = NCLKNDINCS;
}

/******************************************************************************/
// duplicate FAT for FAT16
void duplicateFat(void)
{
	unsigned short i, j;
	unsigned long adr = fatAddr;
	unsigned char *buf = &writeData[0][0];

	if (bit_is_set(PIND, 3)) return;												// Cart�o foi removido

	cmdFast(16, 512);																// Ler 512 bytes
	for (j = 0; j < sectorsPerFat; j++) {
		cmd17Fast(adr);
		for (i=0; i < 512; i++) buf[i] = readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes

		PORTD = NCLKNDI_CS;
		PORTD = NCLKNDINCS;
		
		cmdFast(24, adr + (unsigned long)sectorsPerFat * 512);
		writeByteFast(0xff);
		writeByteFast(0xfe);
		for (i = 0; i < 512; i++) writeByteFast(buf[i]);
		writeByteFast(0xff);
		writeByteFast(0xff);
		readByteFast();
		waitFinish();
		adr += 512;
		
		PORTD = NCLKNDI_CS;
		PORTD = NCLKNDINCS;
	}
}

/******************************************************************************/
// create a file image
int createFile(char *name, char *ext, unsigned short sectNum)
{
	unsigned short re, clusterNum;
	unsigned long ft, adr;
	unsigned short d, i;
	unsigned char c, dirEntry[32], at;
	static unsigned char last[2] = {0xff, 0xff};

	if (bit_is_set(PIND, 3)) return 0;												// Cart�o foi removido
	
	for (i = 0; i < 32; i++) dirEntry[i] = 0;										// Zera estrutura
	memcp(dirEntry, (unsigned char *)name, 8);										// Nome do arquivo
	memcp(dirEntry+8, (unsigned char*)ext, 3);										// Extens�o do arquivo
	*(unsigned long *)(dirEntry + 28) = (unsigned long)sectNum * 512;				// Tamanho em bytes do arquivo
	
	// search a root directory entry
	for (re = 0; re < 512; re++) {
		cmdFast(16, 1);
		cmd17Fast(rootAddr + re * 32 + 0);
		c = readByteFast();															// Primeiro char do nome do arquivo
		readByteFast(); readByteFast(); // discard CRC bytes
		cmd17Fast(rootAddr + re * 32 + 11);
		at = readByteFast();														// Atributo do arquivo
		readByteFast(); readByteFast(); // discard CRC bytes
		if (((c == 0xe5) || (c == 0x00)) && (at != 0xf))							// find a RDE! (Procura uma posi��o vaga)
			break;
	}	
	if (re == 512)																	// N�o achou!! :(
		return 0;
	// write a directory entry
	writeSD(rootAddr + re * 32, dirEntry, 32);
	if (sectNum == 0) {																// Se arquivo tiver zero bytes,
		duplicateFat();																// Duplicar FAT e sair
		return 1;
	}
	// search the first fat entry
	adr = (rootAddr + re * 32 + 26);												// Endere�o do N�mero do cluster inicial (Dentro da entrada de diret�rio)
	clusterNum = 0;
	for (ft=2; (clusterNum < ( ( sectNum + sectorsPerCluster - 1 ) >> sectorsPerCluster2 ) ); ft++) {
		cmdFast(16, 2);
		cmd17Fast(fatAddr + ft * 2);
		d = readByteFast();
		d += (unsigned short)readByteFast() * 0x100;
		readByteFast(); readByteFast(); // discard CRC bytes
		if (d==0) {																	// Se cluster for 0, est� vazio
			clusterNum++;
			writeSD(adr, (unsigned char *)&ft, 2);									// Salva n�mero do cluster vazio
			adr = fatAddr + ft * 2;													// Aponta para o pr�ximo cluster
		}
	}
	writeSD(adr, last, 2);															// Salva 0xFFFF para indicar fim de arquivo
	duplicateFat();
	return 1;
}

/******************************************************************************/
// translate a DSK image into a NIC image
void dsk2Nic(void)
{
	unsigned char trk, logic_sector;

	unsigned short i;
	unsigned char *dst = (&writeData[0][0] + 512);
	unsigned short *fatDsk = (unsigned short *)(&writeData[0][0] + 1024);

	PORTB |= 0b00110000;

	prevFatNumNic = prevFatNumDsk = 0xff;

	for (i = 0; i < 0x16; i++) dst[i] = 0xff;

	// sync header
	dst[0x16] = 0x03;
	dst[0x17] = 0xfc;
	dst[0x18] = 0xff;
	dst[0x19] = 0x3f;
	dst[0x1a] = 0xcf;
	dst[0x1b] = 0xf3;
	dst[0x1c] = 0xfc;
	dst[0x1d] = 0xff;
	dst[0x1e] = 0x3f;
	dst[0x1f] = 0xcf;
	dst[0x20] = 0xf3;
	dst[0x21] = 0xfc;
	
	// address header
	dst[0x22] = 0xd5;
	dst[0x23] = 0xaa;
	dst[0x24] = 0x96;
	dst[0x2d] = 0xde;
	dst[0x2e] = 0xaa;
	dst[0x2f] = 0xeb;
	
	// sync header
	for (i=0x30; i<0x35; i++) dst[i]=0xff;
	
	// data
	dst[0x35] = 0xd5;
	dst[0x36] = 0xaa;
	dst[0x37] = 0xad;
	dst[0x18f] = 0xde;
	dst[0x190] = 0xaa;
	dst[0x191] = 0xeb;
	for (i = 0x192; i < 0x1a0; i++)
		dst[i]=0xff;
	for (i = 0x1a0; i < 0x200; i++)
		dst[i]=0x00;

	cmdFast(16, (unsigned long)512);	
	for (trk = 0; trk < 35; trk++) {
		PORTB ^= 0b00110000; // blink red LED
		for (logic_sector = 0; logic_sector < 16; logic_sector++) {
			unsigned char *src;
			unsigned short ph_sector = (unsigned short)pgm_read_byte_near(physicalSector + logic_sector);

			if (bit_is_set(PIND, 3)) return;														// Cart�o removido

			if ((logic_sector & 1) == 0) {
				unsigned short long_sector = (unsigned short)trk * 8 + (logic_sector / 2);
				unsigned short long_cluster = (long_sector >> sectorsPerCluster2);
				unsigned char fatNum = long_cluster / FAT_DSK_ELEMS;
				unsigned short ft;

				if (fatNum != prevFatNumDsk) {
					prevFatNumDsk = fatNum;						
					prepareFat(dskDir, fatDsk, ((280 + sectorsPerCluster - 1) >> sectorsPerCluster2), fatNum, FAT_DSK_ELEMS);
				}
				ft = fatDsk[long_cluster % FAT_DSK_ELEMS];											// Pega n�mero do cluster do arquivo
				cmd17Fast(
						(unsigned long)userAddr + ( ( (unsigned long)(ft-2) << sectorsPerCluster2) + (long_sector & (sectorsPerCluster - 1) ) ) * (unsigned long)512);
				for (i = 0; i < 512; i++) {
					if (bit_is_set(PIND, 3)) return;
					*(&writeData[0][0] + i) = readByteFast();
				}
				readByteFast(); readByteFast(); // discard CRC bytes				
				src = &writeData[0][0];
			} else {
				src = (&writeData[0][0]+256);
			}
			{
				unsigned char c, x, ox = 0;

				dst[0x25] = ((volume >> 1) | 0xAA);
				dst[0x26] = (volume | 0xAA);
				dst[0x27] = ((trk >> 1) | 0xAA);
				dst[0x28] = (trk | 0xAA);
				dst[0x29] = ((ph_sector >> 1) | 0xAA);
				dst[0x2a] = (ph_sector | 0xAA);
				c = (volume ^ trk ^ ph_sector);
				dst[0x2b] = ((c >> 1) | 0xAA);
				dst[0x2c] = (c | 0xAA);
				for (i = 0; i < 86; i++) {
					x = (pgm_read_byte_near(FlipBit1 + (src[i] & 3)) |
						pgm_read_byte_near(FlipBit2 + (src[i + 86] & 3)) |
						((i <= 83) ? pgm_read_byte_near(FlipBit3 + (src[i + 172] & 3)) : 0));
					dst[i + 0x38] = pgm_read_byte_near(encTable + (x ^ ox));
					ox = x;
				}
				for (i = 0; i < 256; i++) {
					x = (src[i] >> 2);
					dst[i + 0x8e] = pgm_read_byte_near(encTable + (x ^ ox));
					ox = x;
				}
				dst[0x18e] = pgm_read_byte_near(encTable + ox);
			}
			{
				unsigned char c, d;
				unsigned short long_sector = (unsigned short)trk * 16 + ph_sector;
				unsigned short long_cluster = (long_sector >> sectorsPerCluster2);
				unsigned char fatNum = long_cluster / FAT_NIC_ELEMS;
				unsigned short ft;
			
				if (fatNum != prevFatNumNic) {
					prevFatNumNic = fatNum;
					prepareFat(nicDir, fatNic, ((560 + sectorsPerCluster - 1) >> sectorsPerCluster2), fatNum, FAT_NIC_ELEMS);
				}
				ft = fatNic[long_cluster % FAT_NIC_ELEMS];

				PORTD = NCLKNDI_CS;
				PORTD = NCLKNDINCS;

				cmdFast(24, userAddr + (((unsigned long)(ft - 2) << sectorsPerCluster2)
					+ (long_sector & (sectorsPerCluster - 1))) * (unsigned long)512);
				writeByteFast(0xff);
				writeByteFast(0xfe);
				for (i = 0; i < 512; i++) {
					if (bit_is_set(PIND,3)) return;
					c = dst[i];
					for (d = 0b10000000; d; d >>= 1) {
						if (c & d) {
							PORTD = NCLK_DINCS;
							PORTD = _CLK_DINCS;
						} else {
							PORTD = NCLKNDINCS;
							PORTD = _CLKNDINCS;
						}
					}
				}
				PORTD = NCLKNDINCS;
				writeByteFast(0xff);
				writeByteFast(0xff);
				readByteFast();
				waitFinish();
				
				PORTD = NCLKNDI_CS;
				PORTD = NCLKNDINCS;
			}
		}
	}
	buffClear();
	PORTB &= 0b11101111; // off red LED
}

/******************************************************************************/
// make file name list and sort
unsigned short makeFileNameList(unsigned short *list, char *targExt)
{
	unsigned short i, j, k, entryNum = 0;
	char name1[8], name2[8];

	lcd_gotoxy(0, 0);
	lcd_puts_p(MSG5);

	// find extension
	for (i = 0; i != 512; i++) {
		unsigned char ext[3], d;

		if (bit_is_set(PIND, 3)) return 512;										// Cart�o removido
		// check first char
		cmdFast(16, 1);
		cmd17Fast(rootAddr + i * 32);
		d = readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes
		if ((d == 0x00) || (d == 0x05) || (d == 0x2e) || (d == 0xe5))				// Entrada livre
			continue;
		if (!((( d>= 'A') && (d <= 'Z')) || ((d >= '0') && (d <= '9'))))			// Entrada inv�lida
			continue;
		cmd17Fast(rootAddr + i * 32 + 11);
		d = readByteFast();															// Atributo
		readByteFast(); readByteFast(); // discard CRC bytes
		if (d & 0x1e) 																// Arquivo de sistema
			continue;
		if (d == 0xf) 																// Entrada LFN
			continue;
		// check extension
		cmdFast(16, 3);
		cmd17Fast(rootAddr + i * 32 + 8);
		for (j = 0; j != 3; j++)
			ext[j]=readByteFast();
		readByteFast(); readByteFast(); // discard CRC bytes
		if (memcmp(ext, targExt, 3) == 0) {											// Extens�o achada
			list[entryNum++] = i;
		}
	}
	// sort
	if (entryNum > 1)
		for (i = 0; i <= (entryNum - 2); i++) {
			for (j = 1; j <= (entryNum - i - 1); j++) {
				getFileName(list[j], name1);
				getFileName(list[j-1], name2);
			if (memcmp(name1, name2, 8) < 0) {
				k = list[j];
				list[j] = list[j - 1];
				list[j - 1]=k;
			}
		}
	}
	return entryNum;
}

/******************************************************************************/
// choose a NIC file from a NIC file name list
unsigned char chooseANicFile(void *tempBuff, unsigned char btfExists, char *filebase)
{
	unsigned short *list = (unsigned short *)tempBuff;
	unsigned short num = makeFileNameList(list, "NIC");
	char name[8];
	short cur = 0, prevCur = -1;
	unsigned long i;
	unsigned char flagb = 0xFF;

	lcd_gotoxy(0, 0);
	lcd_puts_p(MSG6);

	// if there is at least one NIC file,
	if (num > 0) {
		// determine first file
		if (btfExists) {
			for (i = 0; i < num; i++) {
				getFileName(list[i], name);
				if (memcmp(name, filebase, 8)==0) {
					cur = i;
					break;
				}
			}
		}
		while (1) {
			if (bit_is_set(PIND, 3)) return 0;			// Cart�o foi removido
			if (bit_is_clear(PINB, 5)) {				// up button pushed !
				if (flagb & 0x01) {
					_delay_ms(100);
					while (bit_is_clear(PINB, 5)) nop();
					cur++;
					if (cur == num) cur = 0;
				}
				flagb &= ~0x01;
			} else {
				flagb |= 0x01;
			}

			if (bit_is_clear(PIND, 7)) {				// down button pushed !
				if (flagb & 0x02) {
					_delay_ms(100);
					while (bit_is_clear(PIND, 7)) nop();
					cur--;
					if (cur < 0) cur = num-1;
				}
				flagb &= ~0x02;
			} else {
				flagb |= 0x02;
			}
			if (bit_is_clear(PIND, 6)) {				// enter button pushed !
				unsigned char flg = 1;

				for (i=0; i!=100; i++) if (bit_is_set(PIND, 6)) flg = 0;
				if (flg) {
					while (bit_is_clear(PIND, 6)) nop();
					break;
				}
			}

			// display file name
			if (prevCur != cur) {
				prevCur = cur;

				getFileName(list[cur], name);
				dispStr(name, 1);
			}
			_delay_ms(10);
		}
		getFileName(list[cur], name);
		memcpy(filebase, name, 8);

		return 1;
	} else {
		return 0;
	}
}

/******************************************************************************/
// initialization called from check_eject
void init(unsigned char choose)
{
	unsigned char ch;
	unsigned char i;
	char str[5];
	char filebase[8], btfbase[8];
	unsigned char btfExists, choosen;

	inited = 0;
	PORTB = 0b00110000;	// red LED on

	// initialize the SD card
	PORTD = NCLKNDI_CS;
	for (i = 0; i != 200; i++) {
		PORTD = _CLK_DI_CS;
		wait5(WAIT);
		PORTD = NCLK_DI_CS;
		wait5(WAIT);
	 }	// input 200 clock
 	PORTD = NCLKNDINCS;
	
	cmd_(0, 0);	// command 0
 	do {	
		if (bit_is_set(PIND, 3)) return;												// Cart�o removido
		ch = readByteSlow();
	} while (ch != 0x01);

	PORTD = NCLKNDI_CS;
	while (1) {
		if (bit_is_set(PIND, 3))
			return;
		PORTD = NCLKNDINCS;
		cmd_(55, 0);	// command 55
		ch = getRespSlow();
		if (ch == 0xff) return;
		if (ch & 0xfe) continue;
		// if (ch == 0x00) break;
		PORTD = NCLKNDI_CS;
		PORTD = NCLKNDINCS;
		cmd_(41, 0);	// command 41	
		if (!(ch = getRespSlow()))
			break;
		if (ch == 0xff)
			return;
		PORTD = NCLKNDI_CS;
	}

	// BPB address
	cmdFast(16, 5);
	cmd17Fast(54);
	for (i = 0; i < 5; i++)
		str[i] = readByteFast();
	readByteFast(); readByteFast(); // discard CRC
	if ((str[0] == 'F') && (str[1] == 'A') && (str[2] == 'T') &&
		(str[3] == '1') && (str[4] == '6')) {
		bpbAddr = 0;
	} else {
		cmdFast(16, 4);
		cmd17Fast((unsigned long)0x1c6);
		bpbAddr = readByteFast();
		bpbAddr += (unsigned long)readByteFast()*0x100;
		bpbAddr += (unsigned long)readByteFast()*0x10000;
		bpbAddr += (unsigned long)readByteFast()*0x1000000;
		bpbAddr *= 512;
		readByteFast(); readByteFast(); // discard CRC bytes
	}
	if (bit_is_set(PIND, 3)) return;

	// sectorsPerCluster and reservedSectors
	{
		unsigned short reservedSectors;
		volatile unsigned char k;
		cmdFast(16, 3);
		cmd17Fast(bpbAddr + 0x0D);
		sectorsPerCluster = k = readByteFast();

		sectorsPerCluster2 = 0;
		while (k != 1) {
			sectorsPerCluster2++;
			k >>= 1;
		}

		reservedSectors = readByteFast();
		reservedSectors += (unsigned short)readByteFast()*0x100;
		readByteFast(); readByteFast(); // discard CRC bytes	
		// sectorsPerCluster = 0x40 at 2GB, 0x10 at 512MB
		// reservedSectors = 2 at 2GB
		fatAddr = bpbAddr + (unsigned long)512*reservedSectors;
	}
	if (bit_is_set(PIND, 3)) return;

	{
		// sectorsPerFat and rootAddr
		cmdFast(16, 2);
		cmd17Fast(bpbAddr +0x16);
		sectorsPerFat = readByteFast();
		sectorsPerFat += (unsigned short)readByteFast() * 0x100;
		readByteFast(); readByteFast(); // discard CRC bytes
		// sectorsPerFat =  at 512MB,  0xEF at 2GB
		rootAddr = fatAddr + ((unsigned long)sectorsPerFat * 2 * 512);
		userAddr = rootAddr+(unsigned long)512 * 32;
	}
	if (bit_is_set(PIND, 3)) return;

	// find "BTF" boot file
	btfDir = findExt("BTF", (unsigned char *)0, btfbase, 0);
	btfExists = (btfDir != 512);

	// choose a NIC file from a NIC file list
	if (choose) {
		choosen = chooseANicFile(&writeData[0][0], btfExists, btfbase);
	} else choosen = 0;

	lcd_clear();
	lcd_puts_p(MSG7);

	if (btfExists || choosen)
		memcpy(filebase, btfbase, 8);

	// find "NIC" extension
	nicDir = findExt("NIC", &protect, filebase, btfExists || choosen);

	if (nicDir == 512) { // create NIC file if not exists
		// find "DSK" extension
		dskDir = findExt("DSK", (unsigned char *)0, filebase, btfExists);
		if (dskDir == 512) return;
		if (!createFile(filebase, "NIC", (unsigned short)560)) return;
		nicDir = findExt("NIC", &protect, filebase, btfExists);
		if (nicDir == 512) return;
		// convert DSK image to NIC image
		dsk2Nic();
	}
	if (bit_is_set(PIND, 3)) return;

	// create "BTF" file if not exist
	if (!btfExists) {
		createFile(filebase, "BTF", (unsigned short)0);
		btfDir = findExt("BTF", (unsigned char *)0, filebase, 1);
		btfExists = (btfDir != 512);
	}

	// rewrite the file name part of "BTF"
	if (btfExists && (choosen || (memcmp(filebase, btfbase, 8) != 0))) {
		writeSD(rootAddr + btfDir * 32, (unsigned char *)filebase, 8);
		duplicateFat();
	}

	// display file name
	lcd_clear();
	dispStr(filebase, 0);

	prevFatNumNic = 0xff;
	prevFatNumDsk = 0xff;
	bitbyte = 0;
	readPulse = 0;
	magState = 0;
	prepare = 1;
	ph_track = 0;
	sector = 0;
	buffNum = 0;
	formatting = 0;
	writePtr = &(writeData[buffNum][0]);
	cmdFast(16, (unsigned long)512);
	buffClear();
	inited = 1;
}

/******************************************************************************/
// called when the card is inserted or removed
void check_eject(void)
{
	unsigned long i;
	static unsigned char f = 1;

	if (bit_is_set(PIND, 3)) {
		for (i = 0; i != 0x50000; i++)
			if (bit_is_clear(PIND, 3)) return;
		// SD card removed !
		TIMSK0 &= ~(1<<TOIE0);
		EIMSK &= ~(1<<INT0);
		inited = 0;
		prepare = 0;
		if (f) {
			lcd_clear();
			lcd_puts_p(MSG8);
			f = 0;
		}
	} else if (bit_is_clear(PIND, 6) && bit_is_set(PINC, 0)) { // drive disabled
		unsigned char flg = 1;

		for (i = 0; i != 100; i++)
			if (bit_is_set(PIND,6))
				flg = 0;
		if (flg) {
			while (bit_is_clear(PIND, 6))
				nop();
			// enter button pushed !
			cli();
			init(1);
			if (inited) {
				TIMSK0 |= (1<<TOIE0);
				EIMSK |= (1<<INT0);
			}
			sei();
		}
	} else if (!inited) { // if not initialized
		for (i = 0; i != 0x50000; i++) {
			if (bit_is_set(PIND, 3)) return;
		}
		// SD card inserted !
		cli();
		init(0);
		if (inited) {
			TIMSK0 |= (1<<TOIE0);
			EIMSK |= (1<<INT0);
		}
		sei();
		f = 1;
	}
}

int main(void)
{
	static unsigned char oldStp = 0, stp; // stepper motor input

	/* 1 = OUT, 0 = IN */
	DDRB = 0b00010000;	/* PB4 = LED */
	DDRC = 0b00111010;  /* PC1 = READ PULSE/LCD D4, PC3 = WRITE PROTECT/LCD D5, PC4 = LCD RS, PC5 = LCD E */
	DDRD = 0b00110010;  /* PD1 = SD CS, PD4 = SD DI/LCD D6, PD5 = SD SCK/LCD D7 */

	PORTB = 0b00110000; /* PB4=1 - Led Aceso */
	PORTC = 0b00000010; /* PC4=0 - LCD RS, PC5=0 - LCD Desabilitado */
	PORTD = 0b11000010; /* PD1=0 - SD Desabilitado */

	// timer interrupt
	OCR0A = 0;
	TCCR0A = 0;
	TCCR0B = 1;

	// int0 interrupt
	MCUCR = 0b00000010;
	EICRA = 0b00000010;

	sector = 0;
	inited = 0;
	readPulse = 0;
	protect = 0;
	bitbyte = 0;
	magState = 0;
	prepare = 1;
	ph_track = 0;
	buffNum = 0;
	formatting = 0;
	writePtr = &(writeData[buffNum][0]);

	lcd_init();
	lcd_clear();
	lcd_puts_p(MSG1);
	lcd_gotoxy(0, 1);
	lcd_puts_p(MSG2);
	_delay_ms(1000);

	while (1) {
		check_eject();
		if (bit_is_set(PINC, 0)) {											// disable drive
			PORTB = 0b00100000;												// red LED off
		} else {															// enable drive
			PORTB = 0b00110000;
			// protect = ((PIND&0b10000000)>>4);
			stp = (PINB & 0b00001111);
			if (stp != oldStp) {
				oldStp = stp;
				unsigned char ofs =
					((stp==0b00001000) ? 2 :
					((stp==0b00000100) ? 4 :
					((stp==0b00000010) ? 6 :
					((stp==0b00000001) ? 0 : 0xff))));
				if (ofs != 0xff) {
					ofs = ((ofs+ph_track)&7);
					unsigned char bt = pgm_read_byte_near(stepper_table + (ofs >> 1));
					oldStp = stp;
					if (ofs & 1)
						bt &= 0x0f;
					else
						bt >>= 4;
					ph_track += ((bt & 0x08) ? (0xf8 | bt) : bt);
					if (ph_track > 196)
						ph_track = 0;
					if (ph_track > 139)
						ph_track = 139;
				}
			}
			if (inited && prepare) {
				cli();
				sector = ((sector + 1) & 0xf);
				{
					unsigned char trk = (ph_track >> 2);
					unsigned short long_sector = (unsigned short)trk * 16 + sector;
					unsigned short long_cluster = (long_sector >> sectorsPerCluster2);
					unsigned char fatNum = long_cluster / FAT_NIC_ELEMS;
					unsigned short ft;

					if (fatNum != prevFatNumNic) {
						prevFatNumNic = fatNum;
						prepareFat(nicDir, fatNic, ((560+sectorsPerCluster-1)>>sectorsPerCluster2), fatNum, FAT_NIC_ELEMS);
					}
					ft = fatNic[long_cluster % FAT_NIC_ELEMS];

					if (((sectors[0]==sector)&&(tracks[0]==trk))
						|| ((sectors[1]==sector)&&(tracks[1]==trk))
						|| ((sectors[2]==sector)&&(tracks[2]==trk))
						|| ((sectors[3]==sector)&&(tracks[3]==trk))
						|| ((sectors[4]==sector)&&(tracks[4]==trk))
					) writeBackSub();

					cmd17Fast(userAddr + (((unsigned long)(ft-2) << sectorsPerCluster2)
						+ (long_sector & (sectorsPerCluster - 1))) * 512);
					bitbyte = 0;
					prepare = 0;	
				}	
				sei();
			}
		}
	}
}

/******************************************************************************/
void writeBackSub2(unsigned char bn, unsigned char sc, unsigned char track)
{
	unsigned char c,d;
	unsigned short i;
	unsigned short long_sector = (unsigned short)track*16+sc;
	unsigned short long_cluster = (long_sector>>sectorsPerCluster2);
	unsigned char fatNum = long_cluster/FAT_NIC_ELEMS;
	unsigned short ft;

	if (bit_is_set(PIND, 3)) return;

	if (fatNum != prevFatNumNic) {
		prevFatNumNic = fatNum;
		prepareFat(nicDir, fatNic, ((560 + sectorsPerCluster - 1) >> sectorsPerCluster2), fatNum, FAT_NIC_ELEMS);
	}
	ft = fatNic[long_cluster % FAT_NIC_ELEMS];
	
	PORTD = NCLKNDI_CS;
	PORTD = NCLKNDINCS;

	cmdFast(24, (unsigned long)userAddr + (((unsigned long)(ft - 2) << sectorsPerCluster2) + (unsigned long)(long_sector & (sectorsPerCluster - 1))) * 512);

	writeByteFast(0xff);
	writeByteFast(0xfe);
	// 22 ffs
	for (i = 0; i < 22 * 8; i++) {
		PORTD = NCLK_DINCS;
		PORTD = _CLK_DINCS;
	}
	PORTD = NCLKNDINCS;

	// sync header
	writeByteFast(0x03);
	writeByteFast(0xfc);
	writeByteFast(0xff);
	writeByteFast(0x3f);
	writeByteFast(0xcf);
	writeByteFast(0xf3);
	writeByteFast(0xfc);
	writeByteFast(0xff);
	writeByteFast(0x3f);
	writeByteFast(0xcf);
	writeByteFast(0xf3);
	writeByteFast(0xfc);

	// address header
	writeByteFast(0xd5);
	writeByteFast(0xAA);
	writeByteFast(0x96);
	writeByteFast((volume >> 1) | 0xaa);
	writeByteFast(volume | 0xaa);
	writeByteFast((track >> 1) | 0xaa);
	writeByteFast(track | 0xaa);
	writeByteFast((sc >> 1) | 0xaa);
	writeByteFast(sc | 0xaa);
	c = (volume ^ track ^ sc);
	writeByteFast((c >> 1) | 0xaa);
	writeByteFast(c | 0xaa);
	writeByteFast(0xde);
	writeByteFast(0xAA);
	writeByteFast(0xeb);

	// sync header
	writeByteFast(0xff);	
	writeByteFast(0xff);
	writeByteFast(0xff);
	writeByteFast(0xff);
	writeByteFast(0xff);

	// data
	for (i = 0; i < 349; i++) {
		c = writeData[bn][i];
		for (d = 0b10000000; d; d >>= 1) {
			if (c & d) {
				PORTD = NCLK_DINCS;
				PORTD = _CLK_DINCS;
			} else {
				PORTD = NCLKNDINCS;
				PORTD = _CLKNDINCS;
			}
		}
	}
	PORTD = NCLKNDINCS;
	for (i = 0; i < 14 * 8; i++) {
		PORTD = NCLK_DINCS;
		PORTD = _CLK_DINCS;
	}
	PORTD = NCLKNDINCS;
	for (i = 0; i < 96 * 8; i++) {
		PORTD = NCLKNDINCS;
		PORTD = _CLKNDINCS;
	}
	PORTD = NCLKNDINCS;
	writeByteFast(0xff);
	writeByteFast(0xff);
	readByteFast();
	waitFinish();
	
	PORTD = NCLKNDI_CS;
	PORTD = NCLKNDINCS;
}

/******************************************************************************/
void writeBackSub(void)
{
	unsigned char i, j;

	if (bit_is_set(PIND, 3)) return;
	for (j = 0; j < BUF_NUM; j++) {
		if (sectors[j] != 0xff) {
			for (i = 0; i < BUF_NUM; i++) {
				if (sectors[i] != 0xff)
					writeBackSub2(i, sectors[i], tracks[i]);
				sectors[i] = 0xff;
				tracks[i] = 0xff;
				writeData[i][2]=0;
			}
			buffNum = 0;
			writePtr = &(writeData[buffNum][0]);
			break;
		}
	}
}

/******************************************************************************/
// write back writeData into the SD card
void writeBack(void)
{
	static unsigned char sec;
	
	if (bit_is_set(PIND, 3)) return;
	if (writeData[buffNum][2] == 0xAD) {
		if (!formatting) {
			sectors[buffNum] = sector;
			tracks[buffNum] = (ph_track >> 2);
			sector = ((((sector == 0xf) || (sector == 0xd)) ? (sector + 2) : (sector + 1)) & 0xf);
			if (buffNum == (BUF_NUM - 1)) {
				// cancel reading
				cancelRead();
				writeBackSub();
				prepare = 1;
			} else {
				buffNum++;
				writePtr = &(writeData[buffNum][0]);
			}
		} else {
			sector = sec;
			formatting = 0;
			if (sec == 0xf) {
				// cancel reading
				cancelRead();
				prepare = 1;
			}
		}
	}
	if (writeData[buffNum][2] == 0x96) {
		sec = (((writeData[buffNum][7] & 0x55) << 1) | (writeData[buffNum][8] & 0x55));
		formatting = 1;
	}
}
