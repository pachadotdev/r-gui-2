# these should be listed in the environment pane
x = 1
y <- 1

# this should go right into the plot pane
tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)
