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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DCMD_CLASS_DRIVE              2U
#define DCMD_CURRENT_IG               0U
#define DCMD_SHORT_BRAKE_IG           12U
#define DCMD_DUTY_IG                  16U
#define DCMD_BRAKE_RELEASE            0
#define DCMD_BRAKE_APPLY              1
#define DCMD_COMMAND_TIMEOUT_MS       100U
#define DCMD_FEEDBACK_PERIOD_MS       5U
#define MOTOR_PWM_PERIOD              532U
#define MOTOR_PWM_FULL_ON             (MOTOR_PWM_PERIOD + 1U)

/* Conservative operating limit. The sense circuit can represent about
   32.5 A, but that is not a safe continuous-current rating for the board. */
#define MOTOR_CURRENT_LIMIT_MA        2400

/* CAN-MD.net: DRV8701P SO = VOFF + I * 5 mOhm * 20 V/V. */
#define ADC_FULL_SCALE                4095U
#define ADC_REFERENCE_MV              3300U
#define DRV8701_SENSE_GAIN            20U
#define DRV8701_SHUNT_MILLIOHMS       5U
#define DRV8701_SO_OFFSET_MV          50U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc;

CAN_HandleTypeDef hcan;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static volatile uint8_t dcmd_node;
static volatile uint8_t dcmd_group;
static volatile uint8_t dcmd_brake_group;
static volatile uint8_t dcmd_duty_group;
static volatile uint8_t dcmd_active_group;
static volatile uint8_t estop_latched;
static volatile uint8_t short_brake_latched;
static volatile uint8_t command_received;
static volatile int16_t commanded_current_ma;
static volatile uint32_t last_command_ms;
static uint32_t last_feedback_ms;
static uint16_t current_zero_adc;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CAN_Init(void);
static void MX_TIM2_Init(void);
static void MX_ADC_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
static void CAN_ConfigureFilters(void);
static void Motor_ApplyCurrentCommand(int16_t current_ma);
static void Motor_ApplyDutyCommand(int16_t duty_command);
static void Motor_ApplyShortBrake(void);
static void Motor_Disable(void);
static void DrivePower_Cut(void);
static void DrivePower_Enable(void);
static void Calibrate_CurrentZero(void);
static int16_t Read_Motor_CurrentMa(void);
static void CAN_SendFeedback(void);
static void LED_UpdateBoardNumber(uint8_t board_number, uint32_t now_ms);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void Calibrate_CurrentZero(void)
{
  uint32_t sum = 0U;
  uint32_t samples = 0U;

  for (uint16_t i = 0; i < 128U; i++)
  {
    HAL_ADC_Start(&hadc);
    if (HAL_ADC_PollForConversion(&hadc, 1U) == HAL_OK)
    {
      sum += HAL_ADC_GetValue(&hadc);
      samples++;
    }
  }

  current_zero_adc = (samples > 0U) ? (uint16_t)(sum / samples) : 0U;
}

static int16_t Read_Motor_CurrentMa(void)
{
  uint32_t adc_max = 0;
  uint32_t delta_mv;
  int32_t current_ma;

  /* SO is invalid during the 25 us slow-decay interval. Sample long enough
     to include a drive interval and retain its peak instead of averaging the
     invalid decay samples into the measurement. */
  for (uint8_t i = 0; i < 64U; i++)
  {
    HAL_ADC_Start(&hadc);
    if (HAL_ADC_PollForConversion(&hadc, 1U) == HAL_OK)
    {
      uint32_t adc = HAL_ADC_GetValue(&hadc);
      if (adc > adc_max)
      {
        adc_max = adc;
      }
    }
  }

  if (adc_max <= current_zero_adc)
  {
    current_ma = 0;
  }
  else
  {
    delta_mv = ((adc_max - current_zero_adc) * ADC_REFERENCE_MV +
                (ADC_FULL_SCALE / 2U)) / ADC_FULL_SCALE;
    current_ma = (int32_t)((delta_mv * 1000U) /
                           (DRV8701_SENSE_GAIN *
                            DRV8701_SHUNT_MILLIOHMS));
  }
  if (commanded_current_ma < 0)
  {
    current_ma = -current_ma;
  }
  return (int16_t)current_ma;
}

static void Motor_ApplyCurrentCommand(int16_t current_ma)
{
  int32_t limited_ma = current_ma;
  uint32_t magnitude_ma;
  uint32_t offset_mv;
  uint32_t vref_mv;
  uint32_t vref_compare;

  if (limited_ma > MOTOR_CURRENT_LIMIT_MA)
  {
    limited_ma = MOTOR_CURRENT_LIMIT_MA;
  }
  else if (limited_ma < -MOTOR_CURRENT_LIMIT_MA)
  {
    limited_ma = -MOTOR_CURRENT_LIMIT_MA;
  }

  magnitude_ma = (uint32_t)((limited_ma < 0) ? -limited_ma : limited_ma);
  commanded_current_ma = (int16_t)limited_ma;

  if (magnitude_ma == 0U)
  {
    Motor_Disable();
    return;
  }

  /* DRV8701: ICHOP = (VREF - VOFF) / (AV * RSENSE). */
  offset_mv = ((uint32_t)current_zero_adc * ADC_REFERENCE_MV +
               (ADC_FULL_SCALE / 2U)) / ADC_FULL_SCALE;
  if (current_zero_adc == 0U)
  {
    offset_mv = DRV8701_SO_OFFSET_MV;
  }
  vref_mv = offset_mv +
            ((magnitude_ma * DRV8701_SENSE_GAIN *
              DRV8701_SHUNT_MILLIOHMS + 999U) / 1000U);
  vref_compare = (vref_mv * MOTOR_PWM_PERIOD +
                  (ADC_REFERENCE_MV / 2U)) / ADC_REFERENCE_MV;
  if (vref_compare > MOTOR_PWM_PERIOD)
  {
    vref_compare = MOTOR_PWM_PERIOD;
  }
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, vref_compare);

  /* Hold the requested direction on. DRV8701 performs fixed-off-time
     current chopping internally from the filtered VREF threshold. */
  if (limited_ma > 0)
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, MOTOR_PWM_FULL_ON);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0U);
  }
  else if (limited_ma < 0)
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, MOTOR_PWM_FULL_ON);
  }
}

static void Motor_ApplyDutyCommand(int16_t duty_command)
{
  uint32_t magnitude;
  uint32_t denominator;
  uint32_t duty_compare;
  uint32_t offset_mv;
  uint32_t vref_mv;
  uint32_t vref_compare;

  if (duty_command == 0)
  {
    Motor_Disable();
    return;
  }

  /* Preserve the existing 2.4 A hardware chopping limit while TIM2 applies
     the requested bridge duty. */
  offset_mv = ((uint32_t)current_zero_adc * ADC_REFERENCE_MV +
               (ADC_FULL_SCALE / 2U)) / ADC_FULL_SCALE;
  if (current_zero_adc == 0U)
  {
    offset_mv = DRV8701_SO_OFFSET_MV;
  }
  vref_mv = offset_mv +
            (((uint32_t)MOTOR_CURRENT_LIMIT_MA * DRV8701_SENSE_GAIN *
              DRV8701_SHUNT_MILLIOHMS + 999U) / 1000U);
  vref_compare = (vref_mv * MOTOR_PWM_PERIOD +
                  (ADC_REFERENCE_MV / 2U)) / ADC_REFERENCE_MV;
  if (vref_compare > MOTOR_PWM_PERIOD)
  {
    vref_compare = MOTOR_PWM_PERIOD;
  }
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, vref_compare);

  /* Map the complete signed range to -100 ... +100 percent.  The negative
     endpoint has one extra code, so each polarity uses its own denominator. */
  if (duty_command < 0)
  {
    magnitude = (uint32_t)(-(int32_t)duty_command);
    denominator = 32768U;
  }
  else
  {
    magnitude = (uint32_t)duty_command;
    denominator = 32767U;
  }
  duty_compare = (magnitude * MOTOR_PWM_FULL_ON +
                  (denominator / 2U)) / denominator;
  if (duty_compare > MOTOR_PWM_FULL_ON)
  {
    duty_compare = MOTOR_PWM_FULL_ON;
  }

  /* The command sign selects direction; its magnitude selects PWM duty. */
  /* Read_Motor_CurrentMa uses this variable's sign to report direction. */
  commanded_current_ma = (duty_command > 0) ? 1 : -1;
  if (duty_command > 0)
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty_compare);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0U);
  }
  else
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, duty_compare);
  }
}

static void Motor_Disable(void)
{
  commanded_current_ma = 0;
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0U);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
}

static void DrivePower_Cut(void)
{
  Motor_Disable();
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
}

static void DrivePower_Enable(void)
{
  /* nSLEEP is tied high in CAN-MD.net. Keep VREF at zero until a command. */
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
}

static void Motor_ApplyShortBrake(void)
{
  commanded_current_ma = 0;

  /* DRV8701P PWM interface: IN1 = IN2 = 1 selects low-side
     slow-decay (short-circuit) braking. VREF is unused in this state. */
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, MOTOR_PWM_FULL_ON);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, MOTOR_PWM_FULL_ON);
}

static void LED_UpdateBoardNumber(uint8_t board_number, uint32_t now_ms)
{
  static uint32_t last_transition_ms;
  static uint16_t wait_ms;
  static uint8_t pulse_count;
  static uint8_t led_on;

  if ((uint32_t)(now_ms - last_transition_ms) < wait_ms)
  {
    return;
  }
  last_transition_ms = now_ms;

  if (led_on)
  {
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    led_on = 0U;
    pulse_count++;
    wait_ms = (pulse_count >= board_number) ? 1000U : 150U;
  }
  else
  {
    if (pulse_count >= board_number)
    {
      pulse_count = 0U;
    }
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
    led_on = 1U;
    wait_ms = 150U;
  }
}

static void CAN_ConfigureFilters(void)
{
  CAN_FilterTypeDef filter = {0};
  uint16_t command_id = (uint16_t)((DCMD_CLASS_DRIVE << 8) |
                                   (dcmd_group << 3));
  uint16_t brake_id = (uint16_t)((DCMD_CLASS_DRIVE << 8) |
                                 (dcmd_brake_group << 3));
  uint16_t duty_id = (uint16_t)((DCMD_CLASS_DRIVE << 8) |
                                (dcmd_duty_group << 3));

  filter.FilterBank = 0;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh = 0U;
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = (uint16_t)(0x700U << 5);
  filter.FilterMaskIdLow = 0x0004U; /* Standard class-0 frames; accept any RTR. */
  filter.FilterFIFOAssignment = CAN_RX_FIFO0;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14;
  if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK)
  {
    Error_Handler();
  }

  filter.FilterBank = 1;
  filter.FilterIdHigh = (uint16_t)(command_id << 5);
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
  filter.FilterMaskIdLow = 0x0006U; /* Standard data frames only. */
  if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK)
  {
    Error_Handler();
  }

  filter.FilterBank = 2;
  filter.FilterIdHigh = (uint16_t)(brake_id << 5);
  if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK)
  {
    Error_Handler();
  }

  filter.FilterBank = 3;
  filter.FilterIdHigh = (uint16_t)(duty_id << 5);
  if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK)
  {
    Error_Handler();
  }
}

static void CAN_SendFeedback(void)
{
  CAN_TxHeaderTypeDef header = {0};
  uint8_t data[2];
  uint32_t mailbox;
  int16_t current_ma;

  if ((!command_received) || estop_latched || short_brake_latched ||
      (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U))
  {
    return;
  }

  current_ma = Read_Motor_CurrentMa();
  header.StdId = (uint32_t)((DCMD_CLASS_DRIVE << 8) |
                            (dcmd_active_group << 3) | dcmd_node);
  header.RTR = CAN_RTR_DATA;
  header.IDE = CAN_ID_STD;
  header.DLC = 2U;
  header.TransmitGlobalTime = DISABLE;
  data[0] = (uint8_t)((uint16_t)current_ma >> 8);
  data[1] = (uint8_t)current_ma;
  (void)HAL_CAN_AddTxMessage(&hcan, &header, data, &mailbox);
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
  MX_CAN_Init();
  MX_TIM2_Init();
  MX_ADC_Init();
  MX_USART2_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  uint8_t switch_value = 0U;
  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_RESET) switch_value |= 1U;
  if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) switch_value |= 2U;
  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_RESET) switch_value |= 4U;

  /* Protocol n is 1-based; DIP 000 represents n=8. */
  uint8_t board_number = (switch_value == 0U) ? 8U : switch_value;
  dcmd_group = (uint8_t)(DCMD_CURRENT_IG + ((board_number - 1U) / 4U));
  dcmd_brake_group = (uint8_t)(DCMD_SHORT_BRAKE_IG +
                               ((board_number - 1U) / 4U));
  dcmd_duty_group = (uint8_t)(DCMD_DUTY_IG +
                              ((board_number - 1U) / 4U));
  dcmd_active_group = dcmd_group;
  dcmd_node = (uint8_t)(((board_number - 1U) % 4U) + 1U);
  CAN_ConfigureFilters();

  /* CANの起動 */
  if (HAL_CAN_Start(&hcan) != HAL_OK)
  {
    Error_Handler();
  }

  /* CAN受信割り込み(FIFO0 Pending)の有効化 */
  if (HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
  {
    Error_Handler();
  }

  /* ADCのキャリブレーションを実行 (精度向上のため) */
  HAL_ADCEx_Calibration_Start(&hadc);
  Calibrate_CurrentZero();

  /* Start current-limit VREF and both H-bridge PWM inputs. */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
  Motor_Disable();
  if (estop_latched)
  {
    DrivePower_Cut();
  }
  else if (short_brake_latched)
  {
    Motor_ApplyShortBrake();
  }
  else
  {
    DrivePower_Enable();
  }
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = HAL_GetTick();

    if (estop_latched)
    {
      Motor_Disable();
    }
    else if (short_brake_latched)
    {
      /* The latched bridge state needs no periodic refresh. In particular,
         do not rewrite it here after an E-stop interrupt has disabled it. */
    }
    else if ((!command_received) ||
             ((uint32_t)(now - last_command_ms) >= DCMD_COMMAND_TIMEOUT_MS))
    {
      Motor_Disable();
    }

    if ((uint32_t)(now - last_feedback_ms) >= DCMD_FEEDBACK_PERIOD_MS)
    {
      last_feedback_ms = now;
      CAN_SendFeedback();
    }

    LED_UpdateBoardNumber(board_number, now);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSI14;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSI14State = RCC_HSI14_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI14CalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL4;
  RCC_OscInitStruct.PLL.PREDIV = RCC_PREDIV_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC_Init(void)
{

  /* USER CODE BEGIN ADC_Init 0 */

  /* USER CODE END ADC_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC_Init 1 */

  /* USER CODE END ADC_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc.Instance = ADC1;
  hadc.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc.Init.Resolution = ADC_RESOLUTION_12B;
  hadc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc.Init.ScanConvMode = ADC_SCAN_DIRECTION_FORWARD;
  hadc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc.Init.LowPowerAutoWait = DISABLE;
  hadc.Init.LowPowerAutoPowerOff = DISABLE;
  hadc.Init.ContinuousConvMode = DISABLE;
  hadc.Init.DiscontinuousConvMode = DISABLE;
  hadc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc.Init.DMAContinuousRequests = DISABLE;
  hadc.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  if (HAL_ADC_Init(&hadc) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel to be converted.
  */
  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = ADC_RANK_CHANNEL_NUMBER;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC_Init 2 */

  /* USER CODE END ADC_Init 2 */

}

/**
  * @brief CAN Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN_Init(void)
{

  /* USER CODE BEGIN CAN_Init 0 */

  /* USER CODE END CAN_Init 0 */

  /* USER CODE BEGIN CAN_Init 1 */

  /* USER CODE END CAN_Init 1 */
  hcan.Instance = CAN;
  hcan.Init.Prescaler = 1;
  hcan.Init.Mode = CAN_MODE_NORMAL;
  hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan.Init.TimeSeg1 = CAN_BS1_11TQ;
  hcan.Init.TimeSeg2 = CAN_BS2_4TQ;
  hcan.Init.TimeTriggeredMode = DISABLE;
  hcan.Init.AutoBusOff = ENABLE;
  hcan.Init.AutoWakeUp = DISABLE;
  hcan.Init.AutoRetransmission = ENABLE;
  hcan.Init.ReceiveFifoLocked = DISABLE;
  hcan.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN_Init 2 */

  /* USER CODE END CAN_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 532;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 532;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

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
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : BUTTON_1_Pin BUTTON_3_Pin */
  GPIO_InitStruct.Pin = BUTTON_1_Pin|BUTTON_3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : BUTTON_2_Pin */
  GPIO_InitStruct.Pin = BUTTON_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/**
  * @brief  Rx FIFO 0 msg pending callback.
  * @param  hcan pointer to a CAN_HandleTypeDef structure that contains
  *         the configuration information for the specified CAN.
  * @retval None
  */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef header;
  uint8_t data[8];
  uint16_t id;
  uint32_t dlc;

  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
  {
    return;
  }
  if (header.IDE != CAN_ID_STD)
  {
    return;
  }

  id = (uint16_t)header.StdId;

  /* E-stop deliberately ignores DLC and RTR. */
  if (id == 0x000U)
  {
    estop_latched = 1U;
    short_brake_latched = 0U;
    command_received = 0U;
    DrivePower_Cut();
    return;
  }

  if (id == 0x001U)
  {
    if ((header.RTR == CAN_RTR_DATA) && (header.DLC == 3U) &&
        (data[0] == 0x63U) && (data[1] == 0x6CU) && (data[2] == 0x72U))
    {
      estop_latched = 0U;
      DrivePower_Enable();
    }
    return;
  }

  uint8_t group = (uint8_t)((id >> 3) & 0x1FU);
  if (((id >> 8) != DCMD_CLASS_DRIVE) || estop_latched ||
      (header.RTR != CAN_RTR_DATA) || ((id & 0x07U) != 0U) ||
      ((group != dcmd_group) && (group != dcmd_brake_group) &&
       (group != dcmd_duty_group)))
  {
    return;
  }

  /* Classic CAN carries at most 8 data bytes.  Per protocol, clamp a raw
     DLC above 8 before checking parity and slot availability. */
  dlc = (header.DLC > 8U) ? 8U : header.DLC;
  if ((dlc & 1U) != 0U)
  {
    return;
  }

  if (dlc < (uint32_t)(2U * dcmd_node))
  {
    if ((group != dcmd_brake_group) && (!short_brake_latched))
    {
      Motor_Disable();
    }
    return;
  }

  uint8_t slot = (uint8_t)(2U * (dcmd_node - 1U));
  int16_t command = (int16_t)(((uint16_t)data[slot] << 8) |
                              data[slot + 1U]);

  if (group == dcmd_brake_group)
  {
    if (command == DCMD_BRAKE_APPLY)
    {
      short_brake_latched = 1U;
      command_received = 0U;
      Motor_ApplyShortBrake();
    }
    else if (command == DCMD_BRAKE_RELEASE)
    {
      short_brake_latched = 0U;
      command_received = 0U;
      Motor_Disable();
    }
    return;
  }

  short_brake_latched = 0U;
  dcmd_active_group = group;
  last_command_ms = HAL_GetTick();
  command_received = 1U;
  if (group == dcmd_duty_group)
  {
    Motor_ApplyDutyCommand(command);
  }
  else
  {
    Motor_ApplyCurrentCommand(command);
  }
}
/* USER CODE END 4 */

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
