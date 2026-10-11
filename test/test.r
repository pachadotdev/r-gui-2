setwd('~/Documents/r-gui-2')

# these should be listed in the environment pane
x = 1
y <- 1

iris$Sepal.Length

tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)
