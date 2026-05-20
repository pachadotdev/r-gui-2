#' Initialize the environment monitor
#' @param path Path to the JSON file where environment data should be written
#' @export
init_monitor <- function(path) {
  # Store the path in a package-internal environment or options
  options(qide.env_path = path)
  
  # Remove existing callback if any
  if ("qide_env_monitor" %in% getTaskCallbackNames()) {
    removeTaskCallback("qide_env_monitor")
  }
  
  # Register new callback
  addTaskCallback(function(...) {
    tryCatch({
      update_env()
    }, error = function(e) {
      message("Error in qide monitor: ", e$message)
    })
    return(TRUE)
  }, name = "qide_env_monitor")
  
  # Initial update
  update_env()
  invisible(NULL)
}

#' Update the environment file
#' @export
update_env <- function() {
  path <- getOption("qide.env_path")
  if (is.null(path)) {
    message("qide.env_path is not set")
    return(FALSE)
  }
  
  info <- get_env_info()
  
  # Write to file atomically (write to temp then move? or just write)
  # jsonlite::write_json is convenient
  if (requireNamespace("jsonlite", quietly = TRUE)) {
    tryCatch({
      jsonlite::write_json(info, path, auto_unbox = TRUE, pretty = FALSE)
      # message("Updated environment file: ", path) 
      return(TRUE)
    }, error = function(e) {
      message("Failed to write environment file: ", e$message)
      return(FALSE)
    })
  } else {
    message("jsonlite not available")
    return(FALSE)
  }
}

#' Get environment info as a list
#' @export
get_env_info <- function() {
  objs <- ls(envir = .GlobalEnv)
  
  if (length(objs) == 0) {
    return(list(
      objects = list(),
      types = list(),
      dim = list(),
      len = list()
    ))
  }
  
  vals <- mget(objs, envir = .GlobalEnv)
  
  # Helper to safely get class
  safe_class <- function(x) {
    tryCatch(class(x), error = function(e) "unknown")
  }
  
  # Helper to safely get dim
  safe_dim <- function(x) {
    d <- dim(x)
    if (is.null(d)) return(list()) # Return empty list for JSON {}
    return(d)
  }
  
  # Helper to safely get length
  safe_len <- function(x) {
    tryCatch(length(x), error = function(e) 0)
  }
  
  # Helper to safely get size
  safe_size <- function(x) {
    tryCatch(as.numeric(object.size(x)), error = function(e) 0)
  }
  
  sizes <- lapply(vals, safe_size)
  total_size <- sum(unlist(sizes))
  
  info <- list(
    objects = I(objs),
    types = lapply(vals, safe_class),
    dim = lapply(vals, safe_dim),
    len = lapply(vals, safe_len),
    size = sizes,
    total_size = total_size
  )
  
  return(info)
}

#' Clear the console
#' @export
clear <- function() {
  # In terminal, use ANSI escape sequence or system command
  if (interactive()) {
    cat("\033[2J\033[H")  # ANSI: clear screen and move cursor to home
  }
  invisible(NULL)
}

#' Resolve an R help topic to an httpd URL for the Q help pane
#'
#' Used by the Q IDE C++ frontend. Returns (and optionally writes to a file)
#' a URL served by R's dynamic help httpd (\code{tools::startDynamicHelp}).
#' Only URLs under \code{/doc} or \code{/library} are valid; this function
#' guarantees the returned URL respects that constraint.
#'
#' Resolution strategy:
#' \enumerate{
#'   \item If \code{topic} names an installed package, returns its package
#'         index page (\code{/library/<pkg>/html/00Index.html}).
#'   \item Otherwise calls \code{help()} (bypassing its non-standard
#'         evaluation) and extracts the package + page from the returned
#'         help-db path.
#'   \item Falls back to the R help home page on any error.
#' }
#'
#' @param topic A character scalar (function or package name).
#' @param file  Optional path. If supplied, the URL is written to that file
#'              (one line, atomically) so a file watcher can react.
#' @return The URL string (invisibly when \code{file} is supplied).
#' @export
resolve_help_url <- function(topic, file = NULL) {
  # start = NA → start the httpd if not already running, else return the
  # current port. Using TRUE errors out with "server already running" on
  # some R versions.
  port <- tryCatch(tools::startDynamicHelp(start = NA),
                   error = function(e) tools::startDynamicHelp(start = FALSE))
  home <- sprintf("http://127.0.0.1:%d/doc/html/index.html", port)

  url <- tryCatch({
    topic <- as.character(topic)[1L]

    # 1. Installed package → go straight to its index page.
    if (nzchar(system.file(package = topic))) {
      sprintf("http://127.0.0.1:%d/library/%s/html/00Index.html", port, topic)
    } else {
      # 2. Resolve via help(); bypass NSE by building the call with as.name().
      expr <- tryCatch(as.name(topic), error = function(e) NULL)
      h <- if (!is.null(expr))
             eval(call("help", expr, help_type = "html"))
           else
             character(0)
      paths <- as.character(h)
      if (length(paths) >= 1L) {
        path <- paths[1L]
        pkg  <- basename(dirname(dirname(path)))
        page <- basename(path)
        sprintf("http://127.0.0.1:%d/library/%s/html/%s.html", port, pkg, page)
      } else {
        home
      }
    }
  }, error = function(e) home)

  if (!is.null(file) && nzchar(file)) {
    # In-place write (no atomic rename) so file-system watchers tracking
    # the original inode keep firing on subsequent calls.
    writeLines(url, file)
  }
  invisible(url)
}

#' Initialise the silent help-pane channel
#'
#' Starts the dynamic help server, writes its port to \code{port_file}, and
#' begins polling \code{queue_file} for help-topic requests from the Q IDE
#' frontend. Each line in \code{queue_file} is treated as a topic; the
#' resulting URL is written to \code{url_file}. Polling is done via
#' \code{later::later()} so it runs in R's idle event loop and produces
#' \strong{no} console output. If the \code{later} package is unavailable
#' the function falls back to an \code{addTaskCallback} polling strategy
#' which only fires after the user runs a top-level command.
#'
#' @param port_file  Path where the httpd port should be written.
#' @param queue_file Path the IDE writes topic requests into.
#' @param url_file   Path where the resolved URL is written for the IDE.
#' @param interval   Polling interval in seconds (only used with \code{later}).
#' @export
init_help_pane <- function(port_file, queue_file, url_file, interval = 0.15) {
  # Start the httpd silently. start = NA is idempotent.
  port <- tryCatch(tools::startDynamicHelp(start = NA),
                   error = function(e) tools::startDynamicHelp(start = FALSE))
  try(writeLines(as.character(port), port_file), silent = TRUE)

  process_queue <- function() {
    if (!file.exists(queue_file)) return(invisible(NULL))
    topic <- tryCatch(readLines(queue_file, warn = FALSE),
                      error = function(e) character(0))
    # Remove the request first so a failure doesn't loop forever.
    try(file.remove(queue_file), silent = TRUE)
    topic <- topic[nzchar(topic)]
    if (!length(topic)) return(invisible(NULL))
    try(resolve_help_url(topic[1L], file = url_file), silent = TRUE)
    invisible(NULL)
  }

  if (requireNamespace("later", quietly = TRUE)) {
    poll <- function() {
      tryCatch(process_queue(), error = function(e) NULL)
      later::later(poll, delay = interval)
    }
    later::later(poll, delay = interval)
  } else {
    # Fallback: only fires after each top-level command, but still silent.
    cb_name <- "qide_help_pane"
    if (cb_name %in% getTaskCallbackNames())
      removeTaskCallback(cb_name)
    addTaskCallback(function(...) {
      tryCatch(process_queue(), error = function(e) NULL)
      TRUE
    }, name = cb_name)
  }
  invisible(NULL)
}
