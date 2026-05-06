library(ggplot2)

# these should be listed in the environment pane
x = 1
y <- 1

# this should go right into the plot pane
ggplot(mtcars) +
	geom_point(aes(x = cyl, y = mpg))

g2 <- ggplot(mtcars) +
	geom_point(aes(x = cyl, y = mpg, size = wt))

g2
