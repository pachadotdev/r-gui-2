#' Initialize the environment monitor
#' @param path Path to the JSON file where environment data should be written
#' @export
init_monitor <- function(path) {
  # Store the path in a package-internal environment or options
  options(rgui2.env_path = path)
  
  # Remove existing callback if any
  if ("rgui2_env_monitor" %in% getTaskCallbackNames()) {
    removeTaskCallback("rgui2_env_monitor")
  }
  
  # Register new callback
  addTaskCallback(function(...) {
    tryCatch({
      update_env()
    }, error = function(e) {
      message("Error in rgui2 monitor: ", e$message)
    })
    return(TRUE)
  }, name = "rgui2_env_monitor")
  
  # Initial update
  update_env()
  invisible(NULL)
}

#' Update the environment file
#' @export
update_env <- function() {
  path <- getOption("rgui2.env_path")
  if (is.null(path)) {
    message("rgui2.env_path is not set")
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
      len = list(),
      members = list()
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

  safe_members <- function(x) {
    tryCatch({
      nms <- names(x)
      if (is.null(nms)) character() else unique(nms[nzchar(nms)])
    }, error = function(e) character())
  }

  sizes <- lapply(vals, safe_size)
  total_size <- sum(unlist(sizes))
  visible_names <- unique(unlist(lapply(search(), function(pos) {
    tryCatch(ls(envir = as.environment(pos)), error = function(e) character())
  })))
  visible_vals <- lapply(visible_names, function(name) {
    tryCatch(get(name, envir = .GlobalEnv, inherits = TRUE), error = function(e) NULL)
  })
  member_lists <- lapply(visible_vals, safe_members)
  has_members <- lengths(member_lists) > 0L
  member_lists <- member_lists[has_members]
  member_names <- visible_names[has_members]
  names(member_lists) <- member_names

  info <- list(
    objects = I(objs),
    types = lapply(vals, safe_class),
    dim = lapply(vals, safe_dim),
    len = lapply(vals, safe_len),
    members = member_lists,
    size = sizes,
    total_size = total_size
  )
  
  return(info)
}

#' Initialise plot capture
#'
#' Makes R draw into an off-screen Cairo PNG device. After every top-level
#' command the current plot is re-rendered at the pane size (at 2x scale for
#' sharper output) and sent to the Q IDE over a localhost socket.
#'
#' @param port_file File containing the localhost TCP port used by the Q IDE.
#' @param size_file File containing the requested plot width and height in pixels.
#' @export
init_plot_capture <- function(port_file, size_file) {
  scale <- 2
  png_path <- file.path(tempdir(), "rgui2_plot.png")
  live_path <- file.path(tempdir(), "rgui2_live%03d.png")
  last_signature <- NULL
  last_recorded <- NULL
  save_file <- paste0(size_file, ".save")
  save_result_file <- paste0(size_file, ".saveresult")

  requested_size <- function() {
    size <- c(672L, 480L)
    if (file.exists(size_file)) {
      fields <- suppressWarnings(as.integer(
        strsplit(readLines(size_file, n = 1L, warn = FALSE), " +")[[1L]]))
      if (length(fields) == 2L && !anyNA(fields) && all(fields > 0L))
        size <- pmax(fields, c(320L, 240L))
    }
    size
  }

  open_png <- function(size = requested_size(), file = live_path) {
    grDevices::png(file, width = size[[1L]] * scale,
                   height = size[[2L]] * scale, res = 96 * scale,
                   type = "cairo")
    # File devices don't keep a display list by default, which recordPlot()
    # needs.
    grDevices::dev.control("enable")
  }

  send_png <- function() {
    port <- suppressWarnings(as.integer(readLines(port_file, n = 1L,
                                                   warn = FALSE)))
    bytes <- if (file.exists(png_path)) file.info(png_path)$size else NA_real_
    if (!length(port) || is.na(port) || port <= 0L || is.na(bytes) || bytes <= 0)
      return(invisible(NULL))
    con <- NULL
    tryCatch({
      con <- socketConnection("127.0.0.1", port = port, open = "wb",
                              blocking = TRUE)
      writeBin(charToRaw(sprintf("RGPN %.0f 0\n", bytes)), con)
      writeBin(readBin(png_path, "raw", n = bytes), con)
    }, error = function(e) {
      message("rgui2 plot transfer failed: ", conditionMessage(e))
    }, finally = if (!is.null(con)) close(con))
    invisible(NULL)
  }

  capture_plot <- function(force = FALSE) {
    if (grDevices::dev.cur() == 1L)
      return(invisible(NULL))
    size <- requested_size()
    recorded <- tryCatch(grDevices::recordPlot(), error = function(e) NULL)
    signature <- list(if (!is.null(recorded)) serialize(recorded, NULL), size)
    if (!force && identical(signature, last_signature))
      return(invisible(NULL))
    last_signature <<- signature
    if (!is.null(recorded))
      last_recorded <<- recorded
    if (is.null(recorded))
      return(invisible(NULL))
    # Render the recorded plot on a throw-away device so the live device keeps
    # its state and later calls such as lines() or text() still work.
    live <- grDevices::dev.cur()
    open_png(size, file = png_path)
    grDevices::replayPlot(recorded)
    grDevices::dev.off()
    grDevices::dev.set(live)
    send_png()
    invisible(NULL)
  }

  # Re-draws the last plot natively at the requested size, equivalent to
  # png(file, width, height); <plot>; dev.off().
  save_plot <- function(file, format, width, height) {
    if (is.null(last_recorded))
      stop("there is no plot to save")
    live <- grDevices::dev.cur()
    switch(format,
      png = grDevices::png(file, width = width, height = height, res = 96,
                           type = "cairo"),
      jpg = grDevices::jpeg(file, width = width, height = height, res = 96,
                            quality = 95, type = "cairo"),
      svg = grDevices::svg(file, width = width / 96, height = height / 96),
      stop("unsupported format: ", format))
    on.exit({
      grDevices::dev.off()
      if (live > 1L && live %in% grDevices::dev.list())
        grDevices::dev.set(live)
    })
    grDevices::replayPlot(last_recorded)
    invisible(NULL)
  }

  process_save_request <- function() {
    if (!file.exists(save_file))
      return(invisible(NULL))
    line <- tryCatch(readLines(save_file, n = 1L, warn = FALSE),
                     error = function(e) character())
    try(file.remove(save_file), silent = TRUE)
    if (!length(line) || !nzchar(line))
      return(invisible(NULL))
    # Format: <format>|<width>|<height>|<path>
    fields <- strsplit(line, "|", fixed = TRUE)[[1L]]
    result <- tryCatch({
      if (length(fields) < 4L)
        stop("malformed save request")
      save_plot(paste(fields[-(1:3)], collapse = "|"), fields[[1L]],
                as.integer(fields[[2L]]), as.integer(fields[[3L]]))
      "ok"
    }, error = function(e) paste("error:", conditionMessage(e)))
    writeLines(result, save_result_file)
    invisible(NULL)
  }

  if (requireNamespace("later", quietly = TRUE)) {
    poll_save <- function() {
      tryCatch(process_save_request(), error = function(e) NULL)
      later::later(poll_save, delay = 0.15)
    }
    later::later(poll_save, delay = 0.15)
  }

  options(device = function(width = 7, height = 5, ...) open_png())
  options(rgui2.plot_refresh = function() capture_plot(force = TRUE))

  cb_name <- "rgui2_plot_capture"
  if (cb_name %in% getTaskCallbackNames())
    removeTaskCallback(cb_name)
  addTaskCallback(function(...) {
    tryCatch(process_save_request(), error = function(e) NULL)
    tryCatch(capture_plot(), error = function(e) NULL)
    TRUE
  }, name = cb_name)

  open_png()
  invisible(NULL)
}

#' Re-render the active plot at the current pane dimensions
#' @export
refresh_plot <- function() {
  callback <- getOption("rgui2.plot_refresh")
  if (is.function(callback))
    callback()
  invisible(NULL)
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
  port <- suppressMessages(tryCatch(tools::startDynamicHelp(start = NA),
                   error = function(e) tools::startDynamicHelp(start = FALSE)))
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
  port <- suppressMessages(tryCatch(tools::startDynamicHelp(start = NA),
                   error = function(e) tools::startDynamicHelp(start = FALSE)))
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
    cb_name <- "rgui2_help_pane"
    if (cb_name %in% getTaskCallbackNames())
      removeTaskCallback(cb_name)
    addTaskCallback(function(...) {
      tryCatch(process_queue(), error = function(e) NULL)
      TRUE
    }, name = cb_name)
  }
  invisible(NULL)
}
