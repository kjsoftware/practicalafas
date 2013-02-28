<?php

/**
 * @file
 * NuSOAP Include file for AFAS
 */

/**
 * Returns a nuSOAP client object with all relevant options set.
 * Also does some initial checks.
 *
 * @param $options
 *   Array of options used to set up client object. Keys used:
 *   connector_type / timezone / userId / password.
 *   Also: 'print_errors' determines whether errors will be printed, later.
 *
 * @return bool|nusoap_client
 *   nusoap_client object ready for use (or FALSE if nusoap was not found)
 */
function afas_api_nusoap_connection($options) {

  if (!variable_get('afas_api_environment', '')
      || !variable_get('afas_api_user', '') || !variable_get('afas_api_pw')
      || !$endpoint = trim(variable_get('afas_api_url',
            'https://profitweb.afasonline.nl/profitservices'), '/')) {

    drupal_set_message(t('AFAS web service is not properly configured!'), 'error');
    return FALSE;
  }

  if (!libraries_load('nusoap')) {
    drupal_set_message(t('The required NuSOAP library could not be loaded!'), 'error');
    return FALSE;
  }

  // Add default options if not filled:
  $options += array(
    'userId' => variable_get('afas_api_user'),
    'password' => variable_get('afas_api_pw'),
  );

  // Make sure the aging nuSOAP code does not make PHP5.3 give strict timezone
  // warnings.
  // Note: date_default_timezone_set() is also called in D7's standard
  // drupal_session_initialize() / D8's drupal_set_configured_timezone().
  // So I don't think this is necessary... Still, to be 100% sure:
  if (!ini_get('date.timezone')) {
    if (!$timezone = variable_get('date_default_timezone')) {
      $timezone = @date_default_timezone_get();
    }
    date_default_timezone_set($timezone);
  }

  // Legacy:
  // - if 'connector_type' unset, make it 'get'.
  // - support CREATE for updateconnector too.
  $options += array('connector_type' => 'get');
  if (strtolower($options['connector_type']) == 'create') {
    $options['connector_type'] = 'update';
  }
  // available: get/update/report/subject/dataconnector.
  $endpoint .=  '/' . strtolower($options['connector_type']) . 'connector.asmx';

  $use_wsdl = variable_get('afas_api_use_wsdl', TRUE);
  if ($use_wsdl) {
    $endpoint .= '?WSDL';

    $cache_seconds = variable_get('afas_api_cache_wsdl', 86400);
    if ($cache_seconds) {
      // Get cached WSDL
      $cache = new wsdlcache(file_directory_temp(), $cache_seconds);
      $wsdl  = $cache->get($endpoint);
      if (is_null($wsdl)) {
        $print_errors = isset($connection_options['print_errors']) ? $connection_options['print_errors'] : NULL;
        $wsdl = new wsdl();
        $wsdl->setCredentials('AOL\\' . $options['userId'], $options['password'], 'ntlm');
        $wsdl->fetchWSDL($endpoint);
        if (afas_api_nusoap_handle_client_error($wsdl, $print_errors)) {
          return FALSE;
        }
        $cache->put($wsdl);
      }
      $endpoint = $wsdl;
    }
  }
  $client = new nusoap_client($endpoint, $use_wsdl);
  $client->setCredentials('AOL\\' . $options['userId'], $options['password'], 'ntlm');
  $client->useHTTPPersistentConnection();
  $client->soap_defencoding = 'utf-8';
  $client->xml_encoding = 'utf-8';
  $client->decode_utf8 = FALSE;

  // Specific connection properties can be set by the caller.
  // About timeouts:
  // AFAS has their 'timeout' value on the server set to 5 minutes, and gives no
  // response until it sends the result of a call back. So changing the
  // 'timeout' (default 0) has no effect; the 'response_timeout' can be upped to
  // max. 5 minutes.
  foreach (array(
             'timeout',
             'response_timeout',
             'soap_defencoding',
             'decode_utf8'
           ) as $opt) {

    if (isset($options[$opt])) {
      $client->$opt = $options[$opt];
    }
  }
  return $client;
}

/**
 * Prints or logs NuSOAP client errors.
 *
 * @param nusoap_client $client
 *   NuSOAP client object
 * @param bool $print_errors
 *   TRUE: print client errors and log more info via watchdog.
 *   FALSE: log errors via watchdog, do not print.
 *   NULL/unspecified: use the 'session default value' to determine this.
 *
 * @return bool
 *   TRUE if an error was detected, FALSE otherwise.
 */
function afas_api_nusoap_handle_client_error($client, $print_errors = NULL) {
  if ($error = $client->getError()) {
    $store_error = &drupal_static('afas_api_last_call_info', array());
    $store_error['error'] =  $error;
    $store_error['debug'] = $client->getDebug();
    $store_error['time'] = date('r', time());
    // 'function' and 'arguments' are already present in the static var.

    // Determine whether to print errors, from the function argument OR static var.
    // Always log to watchdog, because that's too much detail to print to the screen.
    watchdog('afas_api_nusoap', 'Error: @error<br/>Function: @function<br/>Arguments: @args',
      array('@error' => $error , '@function' => $store_error['function'],
        '@args' => print_r($store_error['arguments'], TRUE)));
    $print = isset($print_errors) ? $print_errors :
      drupal_static('afas_api_nusoap_print_errors', FALSE);
    if ($print) {
      // You can't just print arguments; one argument may be the full XML which is too long.
      $arg = isset($store_error['arguments'][0]) ? ($store_error['arguments'][0]  . ', ...') : $store_error['arguments'];
      drupal_set_message($store_error['function'] . '(' . $arg . ') ' . $store_error['error']
          . '<br/>See Recent Log Messages for more details.', 'error');
    }
    return TRUE;
  }
  return FALSE;
}

/**
 * 'normalizes' arguments for a certain AFAS SOAP function call.
 * (Not NuSOAP specific.)
 */
function _afas_api_normalize_arguments(&$arguments, $function, $connector_type) {

  // Arguments that only apply to all functions:
  // (try to keep arguments in specified order of function definition.
  // Not that it matters, but it reads better while debugging.)
  $arguments = array_merge(array(
    'environmentId' => variable_get('afas_api_environment'),
    'userId' => variable_get('afas_api_user'),
    'password' => variable_get('afas_api_pw'),
  ), $arguments);

  // Process arguments that only apply to specific functions

  // options
  if ($function == 'GetDataWithOptions') {

    // Turn 'options' argument into XML fragment. Always set options.
    // If $arguments['options'] is not an array, it's silently ignored.
    $options = (isset($arguments['options']) && is_array($arguments['options'])) ?
      $arguments['options'] : array();
    $options += array(
      // From AFAS docs:
      // Outputmode: 1=XML, 2=Text
      'Outputmode' => 1,
      // Metadata: 0=No, 1=Yes
      'Metadata' => 0,
      // Outputoptions: For XML: 2(Microsoft Data set) or 3(Data set including empty values). Default value is 2.
      /* For text, "outputoption 1, 2 ,3 and 4 are valid values, just like in the existing GetData:
        1 = Puntkomma (datums en getallen in formaat van regionale instellingen)
        2 = Tab       (datums en getallen in formaat van regionale instellingen)
        3 = Puntkomma (datums en getallen in vast formaat)
        4 = Tab       (datums en getallen in vast formaat)
        Vast formaat betekent: dd-mm-yy voor datums en punt als decimaal scheidingteken voor getallen."
      */
      'OutputOptions' => 2,
    );
    $options_str = '';
    foreach ($options as $option => $value) {
      $options_str .= "<$option>$value</$option>";
    }
    $arguments['options'] = "<options>$options_str</options>";
  }

  // filtersXML
  if ($function == 'GetDataWithOptions' || $function == 'GetData'
      || ($function == 'Execute' && $connector_type == 'report')) {

    // Turn 'filters' input argument (array) into 'filtersXML' argument accepted
    // by AFAS (XML string).
    // v1 of this module used 'filtersXML' to pass filter arguments - which were
    // not XML. v2 used 'filters'. We now accept both, but will process
    // 'filtersXML' only if 'filters' is empty.
    // If the argument is not an array, it will be silently ignored.
    $filters = array();
    if (!empty($arguments['filters'])) {
      $filters = is_array($arguments['filters']) ? $arguments['filters'] : array();
      unset($arguments['filters']);
    }
    elseif (!empty($arguments['filtersXml'])) {
      $filters = is_array($arguments['filtersXml']) ? $arguments['filtersXml'] : array();
    }
    if ($filters) {

      /* Operators from AFAS documentation:
        1 = Gelijk aan
        2 = Groter dan of gelijk aan
        3 = Kleiner dan of gelijk aan
        4 = Groter dan
        5 = Kleiner dan
        6 = Bevat
        7 = Ongelijk aan
        8 = Moet leeg zijn
        9 = Mag niet leeg zijn
        10 = Begint met
        11 = Bevat niet
        12 = Begint niet met
        13 = eindigt met tekst
        14 = eindigt niet met tekst
      */
      // The non-numeric array values are added by us, to make the input arguments less cryptic.
      // To prevent errors, we'll have several 'signs' resolve to the same op.
      $operators = array(
        '=' => 1,
        '==' => 1,
        '>=' => 2,
        '<=' => 3,
        '>' => 4,
        '<' => 5,
        'LIKE' => 6,      // Note: does NOT resolve to 'starts with'!
        'CONTAINS' => 6,
        '!=' => 7,
        '<>' => 7,
        'NULL' => 8,
        'IS NULL' => 8,
        'NOT NULL' => 9,
        'IS NOT NULL' => 9,
        'STARTS' => 10,
        'STARTS WITH' => 10,
        'NOT LIKE' => 11,
        'NOT CONTAINS' => 11,
        'DOES NOT CONTAIN' => 11,
        'NOT STARTS' => 12,
        'DOES NOT START WITH' => 12,
        'ENDS' => 13,
        'ENDS WITH' => 13,
        'NOT ENDS' => 14,
        'DOES NOT END WITH' => 14,
      );

      /* Different syntaxes for $filters have been introduced over time:
       *
       * 1) array(FIELD1 => VALUE1, ...) - to filter on one or several values.
       *     The simplest one, with a lot of use cases - and one which is
       *     too 'natural' for coders, to stop supporting it.
       *
       * 2) the same, but get the 'operator' from $arguments['filter_operator'].
       *     Is ok, but only allows one and the same operator for all filters.
       *
       * 3) array(
       *     array(FIELD1 => VALUE1, ..., [ '#op' => operator1  ]),
       *     array(FIELD3 => VALUE3, ..., [ '#op' => operator2  ]),
       *    )
       *    This supports multiple operators but is harder to write/read.
       *
       * We want to keep supporting 1 for easier readability (and 2 for backward
       * compatibility), but to prevent strange errors, we'll also support '#op'
       * in the first array level; this overrides 'filter_operator'.
       * Operators can be numeric (AFAS like) as well as the values above.
       * Anything unresolved will default to '='.
       */
      $filters_str = '';
      $operator = !empty($filters['#op']) ? $filters['#op'] : '';
      if (!$operator) {
        $operator = !empty($arguments['filter_operator']) ? $arguments['filter_operator'] : 1;
      }
      if (!is_numeric($operator)) {
        $operator = !empty($operators[$operator]) ? $operators[$operator] : 1;
      }
      foreach ($filters as $outerfield => $filter) {
        if ($outerfield !== '#op') {

          if (is_array($filter)) {
            // Process extra layer
            $op = (!empty($filter['#op'])) ? $filter['#op'] : $operator;
            if (!is_numeric($op)) {
              $op = !empty($operators[$op]) ? $operators[$op] : 1;
            }
            foreach ($filter as $key => $value) {
              if ($key != '#op') {
                $filters_str .= '<Field FieldId="' . $key . '" OperatorType="' . $op . '">' . $value . '</Field>';
              }
            }
          }
          else {
            $filters_str .= '<Field FieldId="' . $outerfield . '" OperatorType="' . $operator . '">' . $filter . '</Field>';
          }
        }
      }
      // There can be multiple 'Filter' tags with FilterID. We only need to use
      // one, it can contain all our filtered fields...
      $arguments['filtersXml'] = '<Filters><Filter FilterId="Filter1">' . $filters_str . '</Filter></Filters>';
    }
  }
}