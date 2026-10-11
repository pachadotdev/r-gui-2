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
#' Installs the compiled Cairo-backed graphics device used by the Q IDE.
#'
#' @param port_file File containing the localhost TCP port used by the Q IDE.
#' @param size_file File containing the requested plot width and height in pixels.
#' @export
init_plot_capture <- function(port_file, size_file) {
  requested_size <- function() {
    if (!file.exists(size_file))
      return(c(0L, 0L))
    fields <- strsplit(readLines(size_file, warn = FALSE)[1L], " +")[[1L]]
    if (length(fields) != 2L)
      return(c(0L, 0L))
    size <- suppressWarnings(as.integer(fields))
    if (anyNA(size) || any(size < 1L))
      return(c(0L, 0L))
    pmax(size, c(320L, 240L))
  }

  # Windows uses the CRAN Cairo package for the file-backed device. This
  # avoids the interactive windows device, which opens an external window.
  if (.Platform$OS.type == "windows") {
    png_path <- file.path(tempdir(), "rgui2_plot.png")
    open_png <- function(path, size, ...) {
      if (!requireNamespace("Cairo", quietly = TRUE))
        stop("The Cairo R package is required for Windows plot capture")
      Cairo::CairoPNG(filename = path, width = size[[1L]], height = size[[2L]],
                      units = "px", dpi = 96, ...)
    }

    send_png <- function(path, size) {
      port <- suppressWarnings(as.integer(readLines(port_file, n = 1L,
                                                     warn = FALSE)))
      file_size <- if (file.exists(path)) file.info(path)$size else NA_real_
      if (!length(port) || is.na(port) || port <= 0L || is.na(file_size)
          || file_size <= 0)
        return(invisible(NULL))

      con <- NULL
      tryCatch({
        con <- socketConnection("127.0.0.1", port = port, open = "wb",
                                blocking = TRUE)
        writeBin(charToRaw(sprintf("RGPN %d 0\n", as.double(file_size))), con)
        writeBin(readBin(path, "raw", n = file_size), con)
      }, error = function(e) {
        message("rgui2 plot transfer failed: ", conditionMessage(e))
      },
      finally = if (!is.null(con)) close(con))
      invisible(NULL)
    }

    reopen_png <- function(size) {
      open_png(png_path, size)
    }

    capture_plot <- function() {
      if (grDevices::dev.cur() == 1L)
        return(invisible(NULL))
      size <- requested_size()
      if (any(size == 0L))
        size <- c(672L, 480L)
      recorded <- tryCatch(grDevices::recordPlot(), error = function(e) NULL)
      grDevices::dev.off()
      if (!is.null(recorded)) {
        reopen_png(size)
        grDevices::replayPlot(recorded)
        grDevices::dev.off()
      }
      send_png(png_path, size)
      reopen_png(size)
      invisible(NULL)
    }

    options(device = function(width = 7, height = 5, ...) {
      size <- requested_size()
      if (any(size == 0L))
        size <- c(round(width * 96), round(height * 96))
      open_png(png_path, size, ...)
    })
    options(rgui2.plot_refresh = capture_plot)
    cb_name <- "rgui2_plot_capture"
    if (cb_name %in% getTaskCallbackNames())
      removeTaskCallback(cb_name)
    addTaskCallback(function(expr, value, ok, visible) {
      tryCatch({
        getOption("rgui2.plot_refresh")()
      }, error = function(e) NULL)
      TRUE
    }, name = cb_name)

    initial_size <- requested_size()
    if (any(initial_size == 0L))
      initial_size <- c(672L, 480L)
    open_png(png_path, initial_size)
    return(invisible(NULL))
  }

  size <- requested_size()
  if (any(size == 0L))
    size <- c(672L, 480L)
  if (grDevices::dev.cur() != 1L)
    try(grDevices::dev.off(), silent = TRUE)
  .Call("rgui2_native_init", normalizePath(port_file, mustWork = FALSE),
        as.integer(size[[1L]]), as.integer(size[[2L]]), PACKAGE = "rgui2")
  options(device = function(width = 7, height = 5, ...) {
    size <- requested_size()
    if (any(size == 0L))
      size <- c(round(width * 96), round(height * 96))
    .Call("rgui2_native_init", normalizePath(port_file, mustWork = FALSE),
          as.integer(size[[1L]]), as.integer(size[[2L]]), PACKAGE = "rgui2")
    invisible(NULL)
  })

  options(rgui2.plot_refresh = function() {
    if (grDevices::dev.cur() == 1L)
      return(invisible(NULL))
    size <- requested_size()
    recorded <- tryCatch(grDevices::recordPlot(), error = function(e) NULL)
    if (!is.null(recorded) && all(size > 0L)) {
      .Call("rgui2_native_resize", as.integer(size[[1L]]),
            as.integer(size[[2L]]), PACKAGE = "rgui2")
      grDevices::replayPlot(recorded)
    }
    invisible(NULL)
  })

  cb_name <- "rgui2_plot_capture"
  if (cb_name %in% getTaskCallbackNames())
    removeTaskCallback(cb_name)

  addTaskCallback(function(expr, value, ok, visible) {
    if (grDevices::dev.cur() != 1L) {
      tryCatch({
        getOption("rgui2.plot_refresh")()
        .Call("rgui2_native_send", PACKAGE = "rgui2")
      }, error = function(e) NULL)
    }
    TRUE
  }, name = cb_name)

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
