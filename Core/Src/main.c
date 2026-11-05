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

#define MAX_SEQUENCE 32

typedef enum
{
    GAME_STATE_WELCOME = 0,
    GAME_STATE_COUNTDOWN,
    GAME_STATE_PLAY_SEQUENCE,
    GAME_STATE_WAIT_INPUT,
    GAME_STATE_CHECK_INPUT,
    GAME_STATE_SUCCESS,
    GAME_STATE_GAME_OVER
} GameState;

GameState gameState = GAME_STATE_WELCOME;

uint8_t sequence[MAX_SEQUENCE];
uint8_t playerHistory[MAX_SEQUENCE];

uint8_t level = 1;
uint8_t inputIndex = 0;
uint8_t historyIndex = 0;
uint8_t currentButton = 0;

uint32_t randomNumber;

GPIO_TypeDef* ledPorts[4] = {
    GPIOC, GPIOC, GPIOD, GPIOC
};

uint16_t ledPins[4] = {
    GPIO_PIN_0,
    GPIO_PIN_1,
    GPIO_PIN_2,
    GPIO_PIN_10
};

GPIO_TypeDef* buttonPorts[4] = {
    GPIOC, GPIOC, GPIOC, GPIOC
};

uint16_t buttonPins[4] = {
    GPIO_PIN_2,
    GPIO_PIN_3,
    GPIO_PIN_4,
    GPIO_PIN_5
};

uint8_t noteNumbers[4] = {0, 4, 7, 11};

uint8_t noteOctave = 2;

uint8_t gameRunning = 1;

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
void Simon_GPIO_Init(void);
void LEDs_AllOff(void);
void LED_On(uint8_t led);
int8_t Read_Button(void);
void Play_Note(uint8_t led);
void Generate_Sequence(void);
void Show_Welcome(void);
void Show_Countdown(void);
void Play_Game_Sequence(void);
void Show_Game_Over(void);
void Show_Success(void);
void Update_Game_Display(void);
void Draw_Game_Buttons(void);
void Add_History(uint8_t button);
void Reset_Game(void);
void Wait_Button_Release(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void Wait_Button_Release(void)
{
    while (Read_Button() != -1)
    {
        HAL_Delay(10);
    }

    HAL_Delay(30);
}

void Simon_GPIO_Init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOCEN;
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIODEN;

    GPIOC->MODER &= ~(GPIO_MODER_MODE0 |
                      GPIO_MODER_MODE1 |
                      GPIO_MODER_MODE10);

    GPIOC->MODER |= GPIO_MODER_MODE0_0 |
                    GPIO_MODER_MODE1_0 |
                    GPIO_MODER_MODE10_0;

    GPIOD->MODER &= ~GPIO_MODER_MODE2;
    GPIOD->MODER |= GPIO_MODER_MODE2_0;

    GPIOC->MODER &= ~(GPIO_MODER_MODE2 |
                      GPIO_MODER_MODE3 |
                      GPIO_MODER_MODE4 |
                      GPIO_MODER_MODE5);

    GPIOC->PUPDR &= ~(GPIO_PUPDR_PUPD2 |
                      GPIO_PUPDR_PUPD3 |
                      GPIO_PUPDR_PUPD4 |
                      GPIO_PUPDR_PUPD5);

    GPIOC->PUPDR |= GPIO_PUPDR_PUPD2_1 |
                    GPIO_PUPDR_PUPD3_1 |
                    GPIO_PUPDR_PUPD4_1 |
                    GPIO_PUPDR_PUPD5_1;

    LEDs_AllOff();
}

void LEDs_AllOff(void)
{
    for (int i = 0; i < 4; i++)
    {
        HAL_GPIO_WritePin(ledPorts[i], ledPins[i], GPIO_PIN_RESET);
    }
}

void LED_On(uint8_t led)
{
    LEDs_AllOff();

    if (led < 4)
    {
        HAL_GPIO_WritePin(ledPorts[led], ledPins[led], GPIO_PIN_SET);
    }
}

int8_t Read_Button(void)
{
    for (int i = 0; i < 4; i++)
    {
        if (HAL_GPIO_ReadPin(buttonPorts[i], buttonPins[i]) == GPIO_PIN_SET)
        {
            return i;
        }
    }

    return -1;
}

void Play_Note(uint8_t led)
{
    if (led < 4)
    {
        buzzerSetNote(&buzzer1, chromaticScale[noteNumbers[led]], noteOctave);
        buzzerEnable(&buzzer1);
    }
}

void Generate_Sequence(void)
{
    if (HAL_RNG_GenerateRandomNumber(&hrng, &randomNumber) == HAL_OK)
    {
        sequence[level - 1] = randomNumber % 4;
    }
    else
    {
        sequence[level - 1] = 0;
    }
}

void Show_Welcome(void)
{
    LEDs_AllOff();
    buzzerDisable(&buzzer1);

    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(2, 2);
    Display_setCursor(35, 25);
    Display_print("SIMON");

    Display_setTextSize(1, 1);
    Display_setCursor(15, 65);
    Display_print("Memory Game");

    Display_setCursor(10, 105);
    Display_print("Press SELECT to start");
}

void Show_Countdown(void)
{
    for (int i = 3; i > 0; i--)
    {
        uint32_t buttons = TFTShield18_readButtons();

        if (!(buttons & TFTSHIELD_BUTTON_IN))
        {
            gameState = GAME_STATE_WELCOME;
            return;
        }

        Display_fillScreen(ST7735_BLACK);
        Display_setTextColor(ST7735_WHITE);
        Display_setTextSize(3, 3);
        Display_setCursor(65, 45);

        char number[2] = {(char)('0' + i), '\0'};
        Display_print(number);

        DWT_delayMs(1000);
    }

    gameState = GAME_STATE_PLAY_SEQUENCE;
}

void Play_Game_Sequence(void)
{
    for (int i = 0; i < level; i++)
    {
        if (!(TFTShield18_readButtons() & TFTSHIELD_BUTTON_IN))
        {
            gameState = GAME_STATE_WELCOME;
            return;
        }

        LED_On(sequence[i]);
        Play_Note(sequence[i]);

        DWT_delayMs(400);

        buzzerDisable(&buzzer1);
        LEDs_AllOff();

        DWT_delayMs(250);
    }

    inputIndex = 0;
    gameState = GAME_STATE_WAIT_INPUT;

    Update_Game_Display();
}

void Add_History(uint8_t button)
{
    playerHistory[historyIndex % MAX_SEQUENCE] = button;
    historyIndex++;
}

void Draw_Game_Buttons(void)
{
    uint16_t colors[4] = {
        ST7735_RED,
        ST7735_BLUE,
        ST7735_GREEN,
        ST7735_YELLOW
    };

    int xPositions[4] = {15, 85, 15, 85};
    int yPositions[4] = {30, 30, 95, 95};

    for (int i = 0; i < 4; i++)
    {
        uint8_t pressed = HAL_GPIO_ReadPin(
            buttonPorts[i],
            buttonPins[i]
        ) == GPIO_PIN_SET;

        Display_fillCircle(
            xPositions[i],
            yPositions[i],
            22,
            pressed ? colors[i] : ST7735_WHITE
        );
    }
}

void Update_Game_Display(void)
{
    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(ST7735_WHITE);
    Display_setTextSize(1, 1);

    Display_setCursor(5, 5);
    Display_print("Level: ");

    char score[4] = {
        (char)('0' + level / 10),
        (char)('0' + level % 10),
        '\0'
    };

    Display_print(score);

    Draw_Game_Buttons();

    Display_setCursor(5, 135);
    Display_print("Last buttons:");

    uint16_t colors[4] = {
        ST7735_RED,
        ST7735_BLUE,
        ST7735_GREEN,
        ST7735_YELLOW
    };

    uint8_t count = historyIndex < 5 ? historyIndex : 5;

    for (int i = 0; i < count; i++)
    {
        uint8_t index = (historyIndex - count + i) % MAX_SEQUENCE;

        Display_fillCircle(
            15 + (i * 25),
            150,
            7,
            colors[playerHistory[index]]
        );
    }
}

void Show_Success(void)
{
    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(ST7735_GREEN);
    Display_setTextSize(2, 2);
    Display_setCursor(10, 45);
    Display_print("Correct!");

    Display_setTextSize(1, 1);
    Display_setCursor(20, 90);
    Display_print("Next level!");

    DWT_delayMs(1000);
}

void Show_Game_Over(void)
{
    Display_fillScreen(ST7735_BLACK);

    Display_setTextColor(ST7735_RED);
    Display_setTextSize(2, 2);
    Display_setCursor(10, 45);
    Display_print("GAME OVER");

    Display_setTextSize(1, 1);
    Display_setCursor(20, 90);
    Display_print("Returning to menu");

    for (int i = 0; i < 20; i++)
    {
        uint32_t buttons = TFTShield18_readButtons();

        if (!(buttons & TFTSHIELD_BUTTON_IN))
            break;

        uint32_t randomValue;

        if (HAL_RNG_GenerateRandomNumber(&hrng, &randomValue) == HAL_OK)
        {
            uint8_t led = randomValue % 4;

            LED_On(led);
            Play_Note(led);
        }

        DWT_delayMs(100);

        LEDs_AllOff();
        buzzerDisable(&buzzer1);
    }

    Reset_Game();
}

void Reset_Game(void)
{
    LEDs_AllOff();
    buzzerDisable(&buzzer1);

    level = 1;
    inputIndex = 0;
    historyIndex = 0;

    gameState = GAME_STATE_WELCOME;
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
  DWT_Init();

  TFTShield18_create(&hi2c1);

  if (!TFTShield18_begin())
  {
      Error_Handler();
  }

  TFTShield18_tftReset(true);
  DWT_delayMs(100);

  Display_create(&hspi2, DISP_CS_Pin, DISP_CS_GPIO_Port,
                 DISP_DCX_SEL_Pin, DISP_DCX_SEL_GPIO_Port);

  Display_init(INITR_BLACKTAB);
  Display_setRotation(1);

  TFTShield18_setBacklight(TFTSHIELD_BACKLIGHT_ON);

  Simon_GPIO_Init();

  buzzerDisable(&buzzer1);

  Show_Welcome();

  while (1)
  {
      switch (gameState)
      {
          case GAME_STATE_WELCOME:
          {
              uint32_t buttons = TFTShield18_readButtons();

              if (!(buttons & TFTSHIELD_BUTTON_IN))
              {
                  level = 1;
                  inputIndex = 0;
                  historyIndex = 0;

                  Generate_Sequence();

                  gameState = GAME_STATE_COUNTDOWN;
              }

              break;
          }

          case GAME_STATE_COUNTDOWN:
          {
              Show_Countdown();
              break;
          }

          case GAME_STATE_PLAY_SEQUENCE:
          {
              Play_Game_Sequence();
              break;
          }

          case GAME_STATE_WAIT_INPUT:
          {
              int8_t button = Read_Button();

              if (button >= 0)
              {
                  currentButton = button;
                  LED_On(currentButton);
                  Play_Note(currentButton);

                  Add_History(currentButton);

                  Update_Game_Display();

                  gameState = GAME_STATE_CHECK_INPUT;
              }

              break;
          }

          case GAME_STATE_CHECK_INPUT:
          {
              if (currentButton == sequence[inputIndex])
              {
                  inputIndex++;

                  Wait_Button_Release();

                  LEDs_AllOff();
                  buzzerDisable(&buzzer1);

                  if (inputIndex >= level)
                  {
                      gameState = SUCCESS;
                  }
                  else
                  {
                      gameState = GAME_STATE_WAIT_INPUT;
                  }
              }
              else
              {
                  gameState = GAME_STATE_GAME_OVER;
              }

              break;
          }

          case GAME_STATE_SUCCESS:
          {
              Show_Success();

              level++;

              if (level >= MAX_SEQUENCE)
              {
                  level = 1;
              }

              Generate_Sequence();

              gameState = GAME_STATE_PLAY_SEQUENCE;

              break;
          }

          case GAME_STATE_GAME_OVER:
          {
              Show_Game_Over();
              Show_Welcome();

              break;
          }

          default:
          {
              Reset_Game();
              Show_Welcome();
              break;
          }
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
