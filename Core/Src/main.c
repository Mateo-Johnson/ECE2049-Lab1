/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "Adafruit_ST7735_Macros.h"
#include "Adafruit_TFTShield18_Macros.h"
#include "dwt_module.h"
#include "Adafruit_TFTShield18_API.h"
#include "Adafruit_ST7735_API.h"
#include "stm32h5xx_hal.h"

#include "buzzer.h"
#include "stm32h5xx_hal_tim.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
#define SET_BUZZER_VOLUME(vol) \ __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (__HAL_TIM_GET_AUTORELOAD(&htim1) * (vol)) / 200)

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

I2C_HandleTypeDef hi2c1;
DMA_HandleTypeDef handle_GPDMA1_Channel6;
DMA_HandleTypeDef handle_GPDMA1_Channel5;

RNG_HandleTypeDef hrng;

SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef handle_GPDMA1_Channel7;

TIM_HandleTypeDef htim1;

/* USER CODE BEGIN PV */
Buzzer buzzer1 = {
  &htim1,
  TIM_CHANNEL_1,
  false
};

extern Note const chromaticScale[NUM_NOTES];

#define SELECT_PRESSED()  ((TFTShield18_readButtons() & TFTSHIELD_BUTTON_IN) == 0)

/* ---- Colors (RGB565) ---- */
#define COLOR_GRAY    0x7BEF
#define COLOR_DKGRAY  0x2104

/* ---- Screen layout (128 x 160, rotation 0): 4 lanes, 32 px each ---- */
#define NUM_LANES       4
#define LANE_W          32
#define SCORE_BAR_H     12
#define TOP_Y           14      /* y where a note appears                    */
#define HIT_Y           120     /* y of a note's top edge at the perfect time*/
#define NOTE_H          10
#define NOTE_W          24
#define NOTE_BOTTOM_Y   140     /* last y a note is drawn at                 */
#define KEY_Y           150     /* row of "key" indicators at the bottom     */
#define KEY_H           9

/* ---- Timing (all in milliseconds) ---- */
#define MS_PER_PX         10                                /* fall speed 0.1 px/ms */
#define TRAVEL_MS         ((HIT_Y - TOP_Y) * MS_PER_PX)     /* spawn -> hit line    */
#define HIT_WINDOW_MS     150     /* +/- this much around the perfect time counts */
#define NOTE_GAP_MS       40      /* silence at end of each note (articulation)   */
#define FRAME_MS          40      /* display refresh period (25 fps)              */
#define SELECT_LOCKOUT_MS 50      /* software debounce for SELECT                 */
#define WIN_PERCENT       60      /* % of notes that must be hit to win           */

/* ---- Song data structure -------------------------------------------------
 * One entry per note: which lane/button, which chromatic note + octave to
 * buzz, and how long until the NEXT note starts. 6 bytes per note after
 * padding -> 32 notes = 192 bytes (const, lives in flash).
 * Lane <-> pitch is fixed, so a given note always uses the same button/color:
 *   lane 0 = G4 (S1)   lane 1 = Bb4 (S2)   lane 2 = C5 (S3)   lane 3 = Db5 (S4)
 * (assumes chromaticScale[0] = C, [1] = C#/Db, ... [7] = G, [10] = A#/Bb)
 * ------------------------------------------------------------------------ */
typedef struct
{
    uint8_t  lane;
    uint8_t  note;        /* index into chromaticScale */
    uint8_t  octave;
    uint16_t durationMs;  /* time from this note's start to the next one's */
} SongNote;

#define G4(d)   { 0,  7, 4, (d) }
#define BB4(d)  { 1, 10, 4, (d) }
#define C5(d)   { 2,  0, 5, (d) }
#define DB5(d)  { 3,  1, 5, (d) }

/* "Smoke on the Water" riff - 32 notes, 4 pitches to the best of my memors */
static const SongNote song[] = {
    /* phrase 1 */ G4(600), BB4(600), C5(900),
    /* phrase 2 */ G4(600), BB4(600), DB5(300), C5(900),
    /* phrase 3 */ G4(600), BB4(600), C5(900), BB4(600), G4(1200),
    /* repeat   */ G4(600), BB4(600), C5(900),
                   G4(600), BB4(600), DB5(300), C5(900),
                   G4(600), BB4(600), C5(900), BB4(600), G4(1200),
    /* ending   */ G4(600), BB4(600), C5(900),
                   G4(600), BB4(600), DB5(300), C5(900),
                   G4(1200)
};
#define SONG_LEN  ((uint8_t)(sizeof(song) / sizeof(song[0])))

static const uint16_t laneColors[NUM_LANES] = {
    ST7735_YELLOW, ST7735_RED, ST7735_BLUE, ST7735_GREEN
};

enum { NOTE_PENDING = 0, NOTE_HIT = 1, NOTE_MISSED = 2 };

typedef enum
{
    GAME_STATE_WELCOME = 0,
    GAME_STATE_COUNTDOWN,
    GAME_STATE_PLAYING,
    GAME_STATE_RESULT
} GameState;

/* ---- State shared between ISR and main loop ---- */
volatile GameState gameState  = GAME_STATE_WELCOME;
volatile uint32_t  g_ms       = 0;     
volatile uint32_t  countdownMs = 0;
volatile uint32_t  songTimeMs  = 0;   
volatile uint8_t   hits = 0, misses = 0;
volatile uint8_t   scoreDirty = 0;    
volatile uint8_t   frameFlag  = 0; 
volatile uint8_t   noteStatus[SONG_LEN];

/* ---- ISR-private song state ---- */
uint32_t noteHitTime[SONG_LEN];     
uint32_t songEndMs = 0;
uint8_t  nextNote = 0;           
uint8_t  missScanIdx = 0;     
uint8_t  prevMask = 0;
uint8_t  frameDiv = 0;
uint8_t  buzzerOn = 0;
uint32_t noteOffAt = 0;

/* ---- Main-loop-private drawing state ---- */
int16_t prevY[SONG_LEN];
uint8_t drawnStatus[SONG_LEN];
uint8_t lastKeyMask = 0xFF;
uint8_t lastCountNum = 0;

/* External LEDs amd buttons */
GPIO_TypeDef* ledPorts[4] = { GPIOD, GPIOC, GPIOB, GPIOC };
uint8_t       ledNums[4]  = { 2, 11, 0, 10 };

GPIO_TypeDef* buttonPorts[4] = { GPIOC, GPIOC, GPIOC, GPIOC };
uint8_t       buttonNums[4]  = { 0, 12, 4, 5 };
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_GPDMA1_Init(void);
static void MX_I2C1_Init(void);
static void MX_ICACHE_Init(void);
static void MX_SPI2_Init(void);
static void MX_RNG_Init(void);
static void MX_TIM1_Init(void);
/* USER CODE BEGIN PFP */
void Game_GPIO_Init(void);
void Game_Timer_Init(void);
void LEDs_Set(uint8_t mask);
void LEDs_AllOff(void);
uint8_t Button_Mask(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void Game_GPIO_Init(void)
{
    RCC->AHB2ENR |= (RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN);
    __IO uint32_t tmpreg = RCC->AHB2ENR;
    (void)tmpreg;

    for (int i = 0; i < 4; i++)
    {
        /* LED pin = output */
        ledPorts[i]->MODER &= ~(3u << (ledNums[i] * 2));
        ledPorts[i]->MODER |=  (1u << (ledNums[i] * 2));

        /* button pin = input with pull-up (pressed reads 0) */
        buttonPorts[i]->MODER &= ~(3u << (buttonNums[i] * 2));
        buttonPorts[i]->PUPDR &= ~(3u << (buttonNums[i] * 2));
        buttonPorts[i]->PUPDR |=  (1u << (buttonNums[i] * 2));
    }
    LEDs_AllOff();
}

void LEDs_Set(uint8_t mask)
{
    for (int i = 0; i < 4; i++)
    {
        if (mask & (1u << i))
            ledPorts[i]->ODR |=  (1u << ledNums[i]);
        else
            ledPorts[i]->ODR &= ~(1u << ledNums[i]);
    }
}

void LEDs_AllOff(void)
{
    LEDs_Set(0);
}

/* REQUIREMENT 1: bit i = 1 when button S(i+1) is pressed (S1 -> 0x01, S3 -> 0x04) */
uint8_t Button_Mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < 4; i++)
    {
        if ((buttonPorts[i]->IDR & (1u << buttonNums[i])) == 0)
            m |= (1u << i);
    }
    return m;
}

/* ---------------------------- TIM2 : 1 ms tick -------------------------- */
void Game_Timer_Init(void)
{
    RCC->APB1LENR |= RCC_APB1LENR_TIM2EN;
    __IO uint32_t tmp = RCC->APB1LENR;
    (void)tmp;

    TIM2->CR1  = 0;
    TIM2->PSC  = (HAL_RCC_GetPCLK1Freq() / 1000000u) - 1u;
    TIM2->ARR  = 1000u - 1u;
    TIM2->EGR  = TIM_EGR_UG;
    TIM2->SR   = ~TIM_SR_UIF;
    TIM2->DIER |= TIM_DIER_UIE;

    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);

    TIM2->CR1 |= TIM_CR1_CEN;
}

/* Timer-based wait, used ONLY once at start-up for the shield reset. */
static void Wait_Ms_Timer(uint32_t ms)
{
    uint32_t start = g_ms;
    while ((g_ms - start) < ms) { }
}

/* ---------------------------- Song / game logic ------------------------- */
static void Song_Init(void)
{
    uint32_t t = TRAVEL_MS;
    for (int i = 0; i < SONG_LEN; i++)
    {
        noteHitTime[i] = t;
        t += song[i].durationMs;
    }
    songEndMs = t;
}

static void Start_Game(void)
{
    for (int i = 0; i < SONG_LEN; i++)
        noteStatus[i] = NOTE_PENDING;

    hits = 0;
    misses = 0;
    songTimeMs = 0;
    nextNote = 0;
    missScanIdx = 0;
    buzzerOn = 0;
    frameDiv = 0;
    frameFlag = 0;
    scoreDirty = 1;
    prevMask = Button_Mask();

    countdownMs = 3000;
    gameState = GAME_STATE_COUNTDOWN;
}

static void Reset_Game(void)
{
    gameState = GAME_STATE_WELCOME;
    buzzerDisable(&buzzer1);
    LEDs_AllOff();
}

static void Judge_Press(uint8_t lane)
{
    uint32_t t = songTimeMs;

    for (uint8_t i = missScanIdx; i < SONG_LEN; i++)
    {
        if (noteHitTime[i] > (t + HIT_WINDOW_MS))
            break;

        if (noteStatus[i] == NOTE_PENDING &&
            song[i].lane == lane &&
            (noteHitTime[i] + HIT_WINDOW_MS) >= t)
        {
            noteStatus[i] = NOTE_HIT;
            hits++;
            scoreDirty = 1;
            return;
        }
    }
}

static void Play_Tick(void)
{
    songTimeMs++;
    uint32_t t = songTimeMs;

    /* --- buzzer: start each note at its hit time, stop it before the next --- */
    if (nextNote < SONG_LEN && t >= noteHitTime[nextNote])
    {
        buzzerSetNote(&buzzer1, chromaticScale[song[nextNote].note], song[nextNote].octave);
        buzzerEnable(&buzzer1);
        noteOffAt = t + song[nextNote].durationMs - NOTE_GAP_MS;
        buzzerOn = 1;
        nextNote++;
    }
    if (buzzerOn && t >= noteOffAt)
    {
        buzzerDisable(&buzzer1);
        buzzerOn = 0;
    }

    /* --- buttons: LEDs follow presses; new presses are judged every 5 ms --- */
    uint8_t mask = Button_Mask();
    LEDs_Set(mask);

    if ((t % 5) == 0)
    {
        uint8_t pressed = mask & (uint8_t)~prevMask;   /* newly pressed only */
        prevMask = mask;
        for (uint8_t lane = 0; lane < NUM_LANES; lane++)
        {
            if (pressed & (1u << lane))
                Judge_Press(lane);
        }
    }

    /* --- notes whose window has fully passed become misses --- */
    while (missScanIdx < SONG_LEN &&
           (noteHitTime[missScanIdx] + HIT_WINDOW_MS) < t)
    {
        if (noteStatus[missScanIdx] == NOTE_PENDING)
        {
            noteStatus[missScanIdx] = NOTE_MISSED;
            misses++;
            scoreDirty = 1;
        }
        missScanIdx++;
    }

    if (++frameDiv >= FRAME_MS)
    {
        frameDiv = 0;
        frameFlag = 1;
    }

    if (t >= songEndMs)
    {
        for (uint8_t i = 0; i < SONG_LEN; i++)
        {
            if (noteStatus[i] == NOTE_PENDING)
            {
                noteStatus[i] = NOTE_MISSED;
                misses++;
            }
        }
        buzzerDisable(&buzzer1);
        buzzerOn = 0;
        LEDs_AllOff();
        gameState = GAME_STATE_RESULT;
    }
}

/* Called from TIM2_IRQHandler every 1 ms */
static void Game_Tick(void)
{
    g_ms++;

    switch (gameState)
    {
        case GAME_STATE_COUNTDOWN:
            if (countdownMs > 0)
                countdownMs--;
            if (countdownMs == 0)
                gameState = GAME_STATE_PLAYING; //2c
            break;

        case GAME_STATE_PLAYING:
            Play_Tick();
            break;

        default:
            break;
    }
}

static uint8_t Select_Event(void)
{
    static uint8_t  prev = 0;
    static uint32_t lastAccepted = 0;

    uint8_t now = SELECT_PRESSED() ? 1 : 0;
    uint8_t ev = 0;

    if (now && !prev && (g_ms - lastAccepted) >= SELECT_LOCKOUT_MS)
    {
        lastAccepted = g_ms;
        ev = 1;
    }
    prev = now;
    return ev;
}
static void Print_Num(uint8_t v)
{
    char b[4];
    uint8_t n = 0;

    if (v >= 100)
    {
        b[n++] = (char)('0' + v / 100);
        v %= 100;
        b[n++] = (char)('0' + v / 10);
    }
    else if (v >= 10)
    {
        b[n++] = (char)('0' + v / 10);
    }
    b[n++] = (char)('0' + v % 10);
    b[n] = '\0';
    Display_print(b);
}
static void Show_Welcome(void)
{
    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(ST7735_YELLOW);
    Display_setTextSize(2, 2);
    Display_setCursor(28, 25);
    Display_print("GUITAR");
    Display_setCursor(40, 45);
    Display_print("HERO");

    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(1, 1);
    Display_setCursor(10, 100);
    Display_print("Press the joystick");
    Display_setCursor(40, 112);
    Display_print("to begin");
}

static void Show_Countdown_Screen(void)
{
    Display_fillScreen(ST7735_BLACK);
    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(1, 1);
    Display_setCursor(36, 30);
    Display_print("Get ready!");
    lastCountNum = 0;
}

static void Update_Countdown(void)
{
    uint8_t num = (uint8_t)((countdownMs + 999u) / 1000u);

    if (num != 0 && num != lastCountNum)
    {
        lastCountNum = num;
        Display_fillRect(44, 56, 40, 40, ST7735_BLACK);
        Display_setTextColor(ST7735_WHITE);
        Display_setTextSize(4, 4);
        Display_setCursor(52, 60);
        char s[2] = { (char)('0' + num), '\0' };
        Display_print(s);
    }
}

static void Draw_Score_Bar(void)
{
    Display_fillRect(0, 0, 128, SCORE_BAR_H, ST7735_BLACK);
    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(1, 1);
    Display_setCursor(2, 2);
    Display_print("Hit ");
    Print_Num(hits);
    Display_print("   Miss ");
    Print_Num(misses);
}
static void Draw_Keys(void)
{
    uint8_t m = Button_Mask();
    if (m == lastKeyMask)
        return;
    lastKeyMask = m;

    for (int i = 0; i < NUM_LANES; i++)
    {
        Display_fillRect(i * LANE_W + 2, KEY_Y, LANE_W - 4, KEY_H,
                         (m & (1u << i)) ? laneColors[i] : COLOR_DKGRAY);
    }
}

static void Draw_Play_Static(void)
{
    Display_fillScreen(ST7735_BLACK);

    for (int i = 1; i < NUM_LANES; i++)
        Display_fillRect(i * LANE_W, TOP_Y, 1, (KEY_Y + KEY_H) - TOP_Y, COLOR_DKGRAY);

    for (int i = 0; i < SONG_LEN; i++)
    {
        prevY[i] = -1;
        drawnStatus[i] = NOTE_PENDING;
    }
    Draw_Score_Bar();
    lastKeyMask = 0xFF;
    Draw_Keys();
}
static void Draw_Play_Frame(void)
{
    uint32_t t = songTimeMs;

    if (scoreDirty)
    {
        scoreDirty = 0;
        Draw_Score_Bar();
    }

    for (int i = 0; i < SONG_LEN; i++)
    {
        uint8_t st = noteStatus[i];
        int16_t wantY = -1;

        if (st != NOTE_HIT)
        {
            int32_t y = HIT_Y + ((int32_t)t - (int32_t)noteHitTime[i]) / MS_PER_PX;
            if (y >= TOP_Y && y <= NOTE_BOTTOM_Y)
                wantY = (int16_t)y;
        }

        int16_t oldY = prevY[i];
        uint8_t x = (uint8_t)(song[i].lane * LANE_W + (LANE_W - NOTE_W) / 2);

        if (oldY >= 0)
        {
            if (wantY < 0)
            {
                Display_fillRect(x, oldY, NOTE_W, NOTE_H, ST7735_BLACK);   /* hit or off-screen */
            }
            else if (wantY > oldY)
            {
                int16_t d = wantY - oldY;
                if (d > NOTE_H) d = NOTE_H;
                Display_fillRect(x, oldY, NOTE_W, d, ST7735_BLACK);        /* uncovered strip */
            }
        }

        if (wantY >= 0 && (wantY != oldY || st != drawnStatus[i]))
        {
            Display_fillRect(x, wantY, NOTE_W, NOTE_H,
                             (st == NOTE_MISSED) ? COLOR_GRAY : laneColors[song[i].lane]);
        }

        prevY[i] = wantY;
        drawnStatus[i] = st;
    }

    Display_fillRect(0, HIT_Y + NOTE_H / 2 - 1, 128, 2, ST7735_WHITE);

    Draw_Keys();
}

/* REQUIREMENT 4: win / lose screen, both show the score */
static void Show_Result(void)
{
    uint8_t h = hits;
    uint8_t m = misses;
    uint8_t pct = (uint8_t)(((uint32_t)h * 100u) / SONG_LEN);
    uint8_t win = (pct >= WIN_PERCENT);

    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(win ? ST7735_GREEN : ST7735_RED);
    Display_setTextSize(2, 2);
    Display_setCursor(16, 20);
    Display_print(win ? "YOU WIN!" : "YOU LOSE");

    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(1, 1);

    Display_setCursor(10, 60);
    Display_print("Hits:     ");
    Print_Num(h);
    Display_print(" / ");
    Print_Num(SONG_LEN);

    Display_setCursor(10, 75);
    Display_print("Misses:   ");
    Print_Num(m);

    Display_setCursor(10, 90);
    Display_print("Accuracy: ");
    Print_Num(pct);
    Display_print("%");

    Display_setCursor(10, 125);
    Display_print("Press SELECT");
    Display_setCursor(10, 137);
    Display_print("to return to menu");
}

static void Enter_Screen(GameState s)
{
    switch (s)
    {
        case GAME_STATE_WELCOME:   Show_Welcome();          break;
        case GAME_STATE_COUNTDOWN: Show_Countdown_Screen(); break;
        case GAME_STATE_PLAYING:   Draw_Play_Static();      break;
        case GAME_STATE_RESULT:    Show_Result();           break;
        default: break;
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_I2C1_Init();
  MX_ICACHE_Init();
  MX_SPI2_Init();
  MX_RNG_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  SET_BUZZER_VOLUME(50);
  DWT_Init();
  Game_GPIO_Init();
  Song_Init();
  Game_Timer_Init();                 /* TIM2 now ticking g_ms every 1 ms */

  TFTShield18_create(&hi2c1);

  if (!TFTShield18_begin())
  {
      Error_Handler();
  }

  TFTShield18_tftReset(true);
  Wait_Ms_Timer(100);                /* one-time start-up wait, measured by TIM2 */

  Display_create(&hspi2, DISP_CS_Pin, DISP_CS_GPIO_Port,
                 DISP_DCX_SEL_Pin, DISP_DCX_SEL_GPIO_Port);

  Display_init(INITR_BLACKTAB);
  Display_setRotation(0);

  TFTShield18_setBacklight(TFTSHIELD_BACKLIGHT_ON);

  int drawnState = -1;               /* force first screen to draw */

  while (1)
  {
      /* SELECT: starts the game from the welcome screen, and from ANY other
         state returns to the welcome screen (REQUIREMENT 2a / 2b / 4b) */
      if (Select_Event())
      {
          if (gameState == GAME_STATE_WELCOME)
              Start_Game();
          else
              Reset_Game();
      }

      GameState s = gameState;

      if ((int)s != drawnState)
      {
          drawnState = (int)s;
          Enter_Screen(s);
      }
      else if (s == GAME_STATE_COUNTDOWN)
      {
          Update_Countdown();
      }
      else if (s == GAME_STATE_PLAYING && frameFlag)
      {
          frameFlag = 0;
          Draw_Play_Frame();
      }
  }
  /* USER CODE END 2 */

  
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSI
                              |RCC_OSCILLATORTYPE_CSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV2;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.CSIState = RCC_CSI_ON;
  RCC_OscInitStruct.CSICalibrationValue = RCC_CSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL1_SOURCE_CSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 45;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 6;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1_VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the programming delay
  */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_0);
}

/**
  * @brief GPDMA1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPDMA1_Init(void)
{

  /* USER CODE BEGIN GPDMA1_Init 0 */

  /* USER CODE END GPDMA1_Init 0 */

  /* Peripheral clock enable */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* GPDMA1 interrupt Init */
    HAL_NVIC_SetPriority(GPDMA1_Channel5_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel5_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel6_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel6_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel7_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel7_IRQn);

  /* USER CODE BEGIN GPDMA1_Init 1 */

  /* USER CODE END GPDMA1_Init 1 */
  /* USER CODE BEGIN GPDMA1_Init 2 */

  /* USER CODE END GPDMA1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00707CBB;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief ICACHE Initialization Function
  * @param None
  * @retval None
  */
static void MX_ICACHE_Init(void)
{

  /* USER CODE BEGIN ICACHE_Init 0 */

  /* USER CODE END ICACHE_Init 0 */

  /* USER CODE BEGIN ICACHE_Init 1 */

  /* USER CODE END ICACHE_Init 1 */

  /** Enable instruction cache (default 2-ways set associative cache)
  */
  if (HAL_ICACHE_Enable() != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ICACHE_Init 2 */

  /* USER CODE END ICACHE_Init 2 */

}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_1LINE;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 0x7;
  hspi2.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  hspi2.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
  hspi2.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
  hspi2.Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
  hspi2.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
  hspi2.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
  hspi2.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
  hspi2.Init.IOSwap = SPI_IO_SWAP_DISABLE;
  hspi2.Init.ReadyMasterManagement = SPI_RDY_MASTER_MANAGEMENT_INTERNALLY;
  hspi2.Init.ReadyPolarity = SPI_RDY_POLARITY_HIGH;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 1400;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 600;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.BreakFilter = 0;
  sBreakDeadTimeConfig.BreakAFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
  sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
  sBreakDeadTimeConfig.Break2Filter = 0;
  sBreakDeadTimeConfig.Break2AFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DISP_DCX_SEL_GPIO_Port, DISP_DCX_SEL_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DISP_CS_GPIO_Port, DISP_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : DISP_DCX_SEL_Pin */
  GPIO_InitStruct.Pin = DISP_DCX_SEL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(DISP_DCX_SEL_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DISP_CS_Pin */
  GPIO_InitStruct.Pin = DISP_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(DISP_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_UIF)
    {
        TIM2->SR = ~TIM_SR_UIF;    /* clear the flag, or the ISR re-fires forever */
        Game_Tick();
    }
}
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  MPU_Attributes_InitTypeDef MPU_AttributesInit = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region 0 and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x08FFF000;
  MPU_InitStruct.LimitAddress = 0x08FFFFFF;
  MPU_InitStruct.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
  MPU_InitStruct.AccessPermission = MPU_REGION_ALL_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Attribute 0 and the memory to be protected
  */
  MPU_AttributesInit.Number = MPU_ATTRIBUTES_NUMBER0;
  MPU_AttributesInit.Attributes = INNER_OUTER(MPU_NOT_CACHEABLE);

  HAL_MPU_ConfigMemoryAttributes(&MPU_AttributesInit);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
